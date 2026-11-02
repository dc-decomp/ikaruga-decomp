#include "game_tweaks.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

#include "decomp.h"

namespace tweaks {
namespace {

render::Tweaks on;  // the tweaks of the last vertical blank, for the wrapped functions below

// The game's data (game/globals.tsv) and the bullets' update functions (game/symbols.tsv).
constexpr uint32_t kPlayersInGame = 0x8C15C588u;  // bit 0: player 1, bit 1: player 2
constexpr uint32_t kPlayerLives = 0x8C1D13ECu;    // uchar[2]
constexpr uint32_t kPlayerEnergy = 0x8C1D13E8u;   // ushort[2], the homing lasers' energy
constexpr uint32_t kGamePrototype = 0x8C15C558u;  // 1 in the prototype mode
constexpr uint32_t kBulletList = 0x8C15C594u + 4 * 8;  // taskLists[4].head
constexpr uint32_t kBulletUpdates[] = {0x8C07CF7Cu /* bulletUpdate */, 0x8C07CB6Cu /* bulletSpriteUpdate */,
                                       0x8C07C874u /* bulletSpinUpdate */};
// A task's header (Task in game/types.h): the removal byte at 1, the update function at 4, the
// next task at 0x14.
constexpr uint32_t kTaskRemoval = 1, kTaskUpdate = 4, kTaskNext = 0x14;

void keep_lives() {
    const uint8_t in_game = guest::load<uint8_t>(kPlayersInGame);
    for (uint32_t i = 0; i < 2; ++i)
        if ((in_game & (1u << i)) && guest::load<uint8_t>(kPlayerLives + i) < 2)
            guest::store<uint8_t>(kPlayerLives + i, 2);
}

// Fills the homing lasers' energy to where absorbing stops (playerAbsorb): 120, or 999 in the
// prototype mode, where the shots spend it too.
void keep_charge() {
    const uint8_t in_game = guest::load<uint8_t>(kPlayersInGame);
    const uint16_t full = guest::load<uint8_t>(kGamePrototype) ? 999 : 120;
    for (uint32_t i = 0; i < 2; ++i)
        if ((in_game & (1u << i)) && guest::load<uint16_t>(kPlayerEnergy + 2 * i) < full)
            guest::store<uint16_t>(kPlayerEnergy + 2 * i, full);
}

// Ends every second bullet that came since the last vertical blank, as taskEnd does. A bullet shows
// for the frame it came in.
void halve_bullets() {
    static std::unordered_set<uint32_t> seen;
    static uint32_t count = 0;
    std::unordered_set<uint32_t> now;
    uint32_t task = guest::load<uint32_t>(kBulletList);
    for (int n = 0; task != 0 && n < 4096; ++n, task = guest::load<uint32_t>(task + kTaskNext)) {
        const uint32_t update = guest::load<uint32_t>(task + kTaskUpdate);
        bool bullet = false;
        for (uint32_t u : kBulletUpdates) bullet |= update == u;
        if (!bullet)
            continue;
        now.insert(task);
        if (!seen.count(task) && guest::load<uint8_t>(task + kTaskRemoval) == 0 && (count++ & 1))
            guest::store<uint8_t>(task + kTaskRemoval, 1);
    }
    seen.swap(now);
}

// hudPressStart's text on a free player's side, which blinks through a whole game for one player:
// empty while the tweak is on.
constexpr uint32_t kPressStartText = 0x8C0D0BDCu;  // "PRESS START"

void hide_press_start(bool on) {
    static bool hidden = false;
    if (on != hidden) {
        guest::store<uint8_t>(kPressStartText, on ? 0 : 'P');
        hidden = on;
    }
}

// The ship's plane: players (Player *[2]), Player.state at 3, flags and forced at 1 and 0 (0x10 lets
// a ship leave the bounds), position at 0x60; the camera (sceneCamera, Camera in game/types.h) with
// its mode at 0, its depth at 8 and its view matrix at 0x30; the camera path that the ships draw in;
// Ninja's projection's scales and centre; the ship's bounds (playerMove's constants); the screen's
// orientation.
constexpr uint32_t kPlayers = 0x8C1D1548u, kStageCamera = 0x8C1D1550u, kPathPosition = 0x8C1D1538u,
                   kPathAngles = 0x8C1D1528u, kPathValue = 0x8C1D1534u, kProjection = 0x8C10AF34u,
                   kBounds = 0x8C076550u, kScreenHorizontal = 0x8C1CB150u;
constexpr uint32_t kCameraMode = 0, kCameraDepth = 8;

using Matrix = std::array<double, 16>;  // Ninja's order: four columns of four

Matrix times(const Matrix& a, const Matrix& b) {
    Matrix c{};
    for (int col = 0; col < 4; ++col)
        for (int row = 0; row < 4; ++row)
            for (int k = 0; k < 4; ++k) c[4 * col + row] += a[4 * k + row] * b[4 * col + k];
    return c;
}

Matrix translation(double x, double y, double z) { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, y, z, 1}; }

// njRotateX, Y and Z: a Ninja angle is 65536 to a turn.
Matrix rotation(int axis, int32_t angle) {
    const double t = static_cast<double>(angle & 0xFFFF) * 2 * M_PI / 65536, s = std::sin(t), c = std::cos(t);
    if (axis == 0)
        return {1, 0, 0, 0, 0, c, s, 0, 0, -s, c, 0, 0, 0, 0, 1};
    if (axis == 1)
        return {c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, 0, 0, 0, 1};
    return {c, s, 0, 0, -s, c, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
}

// Player 1's ship in play, with the view that the game draws it through: the view point of the
// ship's place (x, y) is a + x u + y v. In the camera's mode 0 the ships draw through the camera's
// view, the camera path (pathPosition, then about y, x, -z and y by pathAngles and pathValue) and
// their own position as (x, z, -y) (playerDraw). In mode 1, the flat view of most of the game, they
// draw at (x, y, z - depth) without the camera, turned a quarter on the horizontal screen
// (cameraFlat), so the camera's moves do not move them.
struct Plane {
    uint32_t player = 0;
    double a[3] = {}, u[3] = {}, v[3] = {};
};

double real(uint32_t a) { return static_cast<double>(guest::load<float>(a)); }

bool plane(Plane& p) {
    p.player = guest::load<uint32_t>(kPlayers);
    const uint32_t camera = guest::load<uint32_t>(kStageCamera);
    if (!p.player || !camera || guest::load<uint8_t>(p.player + 3) != 2 ||
        ((guest::load<uint8_t>(p.player + 1) | guest::load<uint8_t>(p.player)) & 0x10))
        return false;
    auto angle = [](uint32_t a) { return guest::load<int32_t>(a); };
    const double z = real(p.player + 0x68);
    Matrix m{};
    if (guest::load<uint8_t>(camera + kCameraMode) == 1) {
        m = guest::load<uint8_t>(kScreenHorizontal) ? translation(0, 0, 0) : rotation(2, -0x4000);
        for (int i = 0; i < 3; ++i) {
            p.u[i] = m[i];
            p.v[i] = m[4 + i];
            p.a[i] = m[8 + i] * (z - guest::load<int32_t>(camera + kCameraDepth)) + m[12 + i];
        }
        return true;
    }
    for (int i = 0; i < 16; ++i) m[i] = real(camera + 0x30 + 4 * i);
    m = times(m, translation(real(kPathPosition), real(kPathPosition + 4), real(kPathPosition + 8)));
    m = times(m, rotation(1, angle(kPathAngles + 4)));
    m = times(m, rotation(0, angle(kPathAngles)));
    m = times(m, rotation(2, -angle(kPathValue)));
    m = times(m, rotation(1, angle(kPathAngles + 8)));
    for (int i = 0; i < 3; ++i) {
        p.u[i] = m[i];
        p.v[i] = -m[8 + i];
        p.a[i] = m[4 + i] * z + m[12 + i];
    }
    return true;
}

// The view point of a ship's place (x, y), and its depth in front of the camera.
double view(const Plane& p, double x, double y, double (&v)[3]) {
    for (int i = 0; i < 3; ++i) v[i] = p.a[i] + x * p.u[i] + y * p.v[i];
    return -v[2];
}

// Where a view point shows in the 640x480 picture.
void picture(const double (&v)[3], double depth, double& px, double& py) {
    px = v[0] / depth * real(kProjection) + real(kProjection + 8);
    py = v[1] / depth * real(kProjection + 4) + real(kProjection + 12);
}

// Puts the ship where the game draws it at the picture's point (px, py). For that point, the view
// point lies on the ray X = -a Z, Y = -b Z, which with a + x u + y v gives two linear equations.
// Where the camera looks across the ships' plane, as in parts of stage 2, a point of the picture
// can lie above the plane's horizon: the ray then meets the plane behind the camera, and that point
// would put the ship on the other side. A ray that meets the plane outside the bounds, or behind
// the camera, takes the ship to the place on the bounds' edge that shows nearest the point.
void place_at(const Plane& p, double px, double py) {
    const double left = real(kBounds), right = real(kBounds + 4), bottom = real(kBounds + 8), top = real(kBounds + 12);
    auto place = [&](double x, double y) {
        guest::store<float>(p.player + 0x60, static_cast<float>(x));
        guest::store<float>(p.player + 0x64, static_cast<float>(y));
    };
    const double a = (px - real(kProjection + 8)) / real(kProjection);
    const double b = (py - real(kProjection + 12)) / real(kProjection + 4);
    const double a11 = p.u[0] + a * p.u[2], a12 = p.v[0] + a * p.v[2], r1 = -(p.a[0] + a * p.a[2]);
    const double a21 = p.u[1] + b * p.u[2], a22 = p.v[1] + b * p.v[2], r2 = -(p.a[1] + b * p.a[2]);
    const double det = a11 * a22 - a12 * a21;
    if (std::abs(det) > 1e-9) {
        const double x = (r1 * a22 - a12 * r2) / det, y = (a11 * r2 - r1 * a21) / det;
        double v[3];
        if (view(p, x, y, v) > 0 && x >= left && x <= right && y >= bottom && y <= top) {
            place(x, y);
            return;
        }
    }
    // The point's distance in the picture from where a ship's place shows.
    auto distance = [&](double x, double y) {
        double v[3], sx, sy;
        const double depth = view(p, x, y, v);
        if (depth <= 1e-6)
            return HUGE_VAL;
        picture(v, depth, sx, sy);
        return std::hypot(sx - px, sy - py);
    };
    // Along each edge, the distance falls and then rises, as the edge is a straight line in the
    // picture too.
    const double edges[4][4] = {
        {left, bottom, right, bottom}, {right, bottom, right, top}, {right, top, left, top}, {left, top, left, bottom}};
    double best = HUGE_VAL, best_x = 0, best_y = 0;
    for (const auto& e : edges) {
        double lo = 0, hi = 1;
        for (int i = 0; i < 40; ++i) {
            const double s1 = lo + (hi - lo) / 3, s2 = hi - (hi - lo) / 3;
            if (distance(e[0] + s1 * (e[2] - e[0]), e[1] + s1 * (e[3] - e[1])) <
                distance(e[0] + s2 * (e[2] - e[0]), e[1] + s2 * (e[3] - e[1])))
                hi = s2;
            else
                lo = s1;
        }
        const double s = (lo + hi) / 2, x = e[0] + s * (e[2] - e[0]), y = e[1] + s * (e[3] - e[1]);
        if (const double d = distance(x, y); d < best) {
            best = d;
            best_x = x;
            best_y = y;
        }
    }
    if (best < HUGE_VAL)
        place(best_x, best_y);
}

}  // namespace

// From where the ship shows now, so that the camera's moves carry it as they carry a ship that a
// gamepad steers, and the places that the game gives it, at a chapter's start or after a loss, hold.
// The tutorial and the attract demo play recorded input, which steers the ship alone.
void ship_by(float dx, float dy) {
    Plane p;
    if ((dx == 0 && dy == 0) || demoInput != 0 || !plane(p))
        return;
    double v[3], px, py;
    const double depth = view(p, real(p.player + 0x60), real(p.player + 0x64), v);
    if (depth <= 1e-6)
        return;
    picture(v, depth, px, py);
    place_at(p, px + dx, py + dy);
}

render::Tweaks from_environment() {
    render::Tweaks t;
    if (const char* v = std::getenv("IKARUGA_TWEAKS")) {
        t.infinite_lives = std::strstr(v, "lives") != nullptr;
        t.infinite_charge = std::strstr(v, "charge") != nullptr;
        t.half_bullets = std::strstr(v, "bullets") != nullptr;
        t.no_press_start = std::strstr(v, "start") != nullptr;
        t.instant_switch = std::strstr(v, "switch") != nullptr;
        t.no_music = std::strstr(v, "music") != nullptr;
        t.no_death_bullets = std::strstr(v, "death") != nullptr;
        t.english = std::strstr(v, "english") != nullptr;
    }
    return t;
}

const render::Tweaks& current() { return on; }

void apply(const render::Tweaks& t) {
    on = t;
    if (t.infinite_lives)
        keep_lives();
    if (t.infinite_charge)
        keep_charge();
    if (t.half_bullets)
        halve_bullets();
    hide_press_start(t.no_press_start);
}

}  // namespace tweaks

namespace game {

// SWITCH AT ONCE (port/hooked.txt). playerMove reads the buttons only while no turn runs, and turns
// the ship over 14 frames: the color changes at 0x2400, and until then bullets of both colors hit
// the ship (playerButtons gives it the hit value 's'). With the tweak, the wrapper runs the turn and
// shows playerMove none, so the buttons work in every frame, and a switch changes the color at once.
// The attract demo plays the game's own switch, which its recorded input expects.
void playerMove(gptr<Player> player) {
    // Each player's turn that changed the color at its start, which the game's turn must not repeat.
    static bool changed[2] = {};
    bool& early = changed[player->index & 1];
    if (!tweaks::on.instant_switch || demoInput != 0) {
        // A turn that changed the color at its start skips the game's change halfway.
        if (early && player->turn != 0 && player->turn < 0x2400)
            player->turn = 0x2400;
        early = false;
        return playerMove_game(player);
    }
    uint16_t turn = player->turn;
    if (turn != 0) {
        // A turn that the game started before the tweak was on changes the color now.
        if (!early && turn < 0x2400) {
            player->polarity = static_cast<uchar>(player->polarity ^ 1);
            playerSetPolarity(player);
            playerTurnEffect(player->index);
        }
        early = true;
        turn = static_cast<uint16_t>(turn + 0x400);
        if (turn == 0x3c00)
            player->flags = static_cast<uchar>(player->flags & 0xdf);
        if (turn == 0x4000) {
            player->turnRoll = 0;
            turn = 0;
        }
        else {
            const Float s = njSin(turn);
            player->turnRoll = static_cast<int>(fmac(s, static_cast<float>(player->turnRollTo),
                                                     static_cast<float>(player->turnRollFrom) * (1.0f - s)));
        }
    }
    const int roll = player->turnRoll;
    player->turn = 0;
    playerMove_game(player);
    if (player->turn == 0) {
        player->turn = turn;
        early = turn != 0;
        return;
    }
    // playerButtons started a turn: the new color now, and a turn that ran goes on from its roll.
    player->turnRollFrom = roll;
    player->polarity = static_cast<uchar>(player->polarity ^ 1);
    playerSetPolarity(player);
    playerTurnEffect(player->index);
    early = true;
}

// NO MUSIC (port/hooked.txt). The music plays as MIDI sequences, whose volume the SDK's sdMidiSetVol
// sets: at their start in soundServer, and for a fade. With the tweak it sets -127, the volume at
// which the game itself silences an effect. The sequences still play, so the game's timing stays.
namespace {
std::unordered_map<uint32_t, char> asked_volume;  // by sequence handle, what the game set last
bool music_muted = false;
}  // namespace

dword sdMidiSetVol(gptr<undefined1> handle, char volume, int fade) {
    asked_volume[handle.a] = volume;
    return sdMidiSetVol_game(handle, tweaks::on.no_music ? static_cast<char>(-0x7f) : volume, fade);
}

// A change of the tweak reaches the sequences that play: soundSequencePlayers holds a handle for
// each of the 8 slots, 0x20 bytes apart.
void soundServer() {
    if (tweaks::on.no_music != music_muted) {
        music_muted = tweaks::on.no_music;
        for (uint32_t i = 0; i < 8; ++i) {
            const gptr<undefined1> handle = *gptr<gptr<undefined1>>(0x8c1cb22cu + 0x20 * i);
            if (soundSequenceSlots[i].state == 0 || !handle)
                continue;
            const auto it = asked_volume.find(handle.a);
            const char volume = it != asked_volume.end() ? it->second : static_cast<char>(soundSequenceSlots[i].level);
            sdMidiSetVol_game(handle, music_muted ? static_cast<char>(-0x7f) : volume, 0);
        }
    }
    soundServer_game();
}

// NO DEATH BULLETS (port/hooked.txt): the bullets of the enemy's color that a destroyed enemy fires
// at the player who made the kill. The attract demo keeps them, as its recorded input expects them.
void enemyDeathBullets(int enemy, gptr<float> position, uint count, float spread, float speed, float param_6) {
    if (tweaks::on.no_death_bullets && demoInput == 0)
        return;
    enemyDeathBullets_game(enemy, position, count, spread, speed, param_6);
}

}  // namespace game
