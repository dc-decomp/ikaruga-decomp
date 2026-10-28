// The game's lines of text (port/hooked.txt). FUN_8c06ab78 makes a texture of a line of Shift-JIS
// characters, 24 dots each from the boot ROM's font (FUN_8c06aa40), which FUN_8c06aaa0 copies in
// with each dot also on the dots to its right and below.
//
// Where the port draws the glyphs itself, they blend at their edges: FUN_8c06aaa0's wrapper writes
// how much of each dot the character covers as the alpha of an ARGB4444 texture. The game's own
// pixels mean the same in that format, its white 0xFFFE and its empty 0x0001.
//
// ENGLISH TEXT: for a line that the table below knows, the wrapper draws the English line with
// Hiragino Sans (render/font.h), cuts it into 24-dot cells, and gives the game a line of as many
// codes from Shift-JIS's user area, whose glyphs are the cells. The game then sizes, loads and
// places the texture as it does for the Japanese line.
#include "text.h"

#include <algorithm>
#include <unordered_map>
#include <vector>

#include "decomp.h"
#include "diff.h"
#include "game_tweaks.h"
#include "render/font.h"

namespace {

// The cells' codes: lead byte 0xF0, trail bytes from 0x40, below 0x7F, which Shift-JIS skips.
constexpr uint16_t kFirstCell = 0xF040;
constexpr size_t kMaxCells = 0x7F - 0x40;

// The game's lines by the FNV-1a hash of their Shift-JIS bytes, so that none of the game's text is
// here. tools/port/english.py lists the lines of the disc's program with their hashes.
const std::unordered_map<uint32_t, const char*> kEnglish = {
    // The save file's title, which only the console's own file manager shows.
    {0x1f9462a0u, "Ikaruga"},
    // The autosave's failure, and the disc's read error (discErrorScreen).
    {0x7ac36b04u, "The autosave failed."},
    {0xb6e6106fu, "Autosave is now off, so"},
    {0x9d00d14eu, "please save the game yourself."},
    {0xd730c9f2u, "The disc could not be read."},
    {0x6ef29df0u, "Turn the console off and start it again."},
    // The pause menu.
    {0x06c92382u, "Resume the game."},
    {0x3307ef58u, "Adjust where the score and the gauges appear."},
    {0x06e39c85u, "Quit the game and return to the title screen."},
    {0x764d62d9u, "Restart from the beginning of the stage."},
    {0x44a526a9u, "A: Reset the position   B: Back"},
    // The CONFIG menu.
    {0xbb0645f5u, "Set the type of monitor."},
    {0xd049d199u, "Set the sound output."},
    {0x7071dfebu, "Set the difficulty."},
    {0x17a0cc5cu, "Turn the trial game on or off."},
    {0x216fe5c6u, "Set when you earn an extra ship."},
    {0xfce8f592u, "Set the number of ships."},
    {0xe3a131d4u, "Turn autosave on or off."},
    {0xa34f0bf6u, "Do not remove the memory card."},
    {0x75c49b0cu, "Set the game mode."},
    {0x17fa1ec0u, "Return to the previous screen."},
    {0xe84d688du, "Go to the controller settings."},
    {0xefc23285u, "Restore the default settings."},
    {0x22b1b24bu, "Now: Horizontal 2 (scaled to 80%)"},
    {0x3f9c58f6u, "Now: Horizontal 1 (scaled to 75%)"},
    {0x2873ad98u, "Now: Vertical 1 (directions not turned)"},
    {0x03a030c0u, "Now: Vertical 2 (directions turned)"},
    {0x14881ad7u, "Now: Mono"},
    {0x76ef47e4u, "Now: Stereo"},
    {0x0a334259u, "Now: Easy"},
    {0x3a12ae97u, "Now: Normal"},
    {0xbff37aeau, "Now: Hard"},
    {0x024342d6u, "Now: Select (choose at the start of a game)"},
    {0x3f37ed64u, "Now: Trial game on"},
    {0x1e37b971u, "Now: Trial game off"},
    {0x677a4e28u, "Now: No extra ships"},
    {0x6e8c44adu, "Now: Every 2,000,000 points"},
    {0x3fc824bbu, "Now: At 3,000,000, then every 5,000,000 points"},
    {0x34880054u, "Now: At 6,000,000 and at 15,000,000 points"},
    {0x5414f294u, "Now: 1"},
    {0x5514f427u, "Now: 2"},
    {0x5614f5bau, "Now: 3"},
    {0x5714f74du, "Now: 4"},
    {0x5014ec48u, "Now: 5"},
    {0x2bf2f4e1u, "Now: Autosave off"},
    {0xccf3f254u, "Now: Autosave on"},
    {0x641eab28u, "Now: Arcade mode"},
    {0xf8096940u, "Now: Prototype mode (limited shots)"},
    // The controller settings' help.
    {0xa24eb5fcu, "From the top, this page sets"},
    {0xfeb9d6acu, "the buttons for shot, switch and release,"},
    {0xf548ce9bu, "release with shot and switch together on or off,"},
    {0x61ec1364u, "and vibration on or off."},
    {0xa84e3e91u, ""},
    {0xb86124a9u, "EXIT returns to the previous screen."},
    // The main menu.
    {0xa093f3bbu, "Start the game."},
    {0x915bf8c8u, "Choose a stage to practice."},
    {0xf2f7930eu, "View or reset the rankings."},
    {0xab575314u, "Change the game's settings."},
    {0x06208019u, "A demo that explains the game."},
    {0x21529671u, "Save or load the data file,"},
    {0x78460a94u, "and turn autosave on or off."},
    {0xaa73fd8fu, "Return to the title screen."},
    {0x404946a6u, "Extras."},
    {0x2babbaebu, "Choose the stage to practice."},
    {0x8b8d74cdu, "You have not reached this stage yet."},
    // The rankings.
    {0xf0bbe977u, "View the rankings."},
    {0x7e1556f1u, "Reset the rankings."},
    {0xd8f9f6f4u, "*** The rankings will be reset! ***"},
    {0xdfedf3f6u, "Are you sure?"},
    {0xc6ea71fau, "The rankings have been reset."},
    // Saving and loading.
    {0xb7cb8ad9u, "* A data file is there already! *"},
    {0xda26e3e5u, "Overwrite it?"},
    {0xb715da1bu, "Load the data file."},
    {0x23d981a9u, "Save the data file."},
    {0x652878b3u, "* Saving the data file *"},
    {0x2eba2216u, "The data file was saved."},
    {0x34742bc0u, "The data file could not be saved."},
    {0xbb28d351u, "* Loading the data file *"},
    {0xfe3fb368u, "The data file was loaded."},
    {0x11d7b41eu, "The data file could not be loaded."},
    // The extras and what unlocks them.
    {0xc9b99ee6u, "Listen to the game's music."},
    {0xb97b3cb3u, "To unlock: clear on Easy with one credit"},
    {0x06796476u, "or play for 15 hours"},
    {0xeab038e1u, "To unlock: clear on Normal with one credit"},
    {0x6ee67780u, "or play for 20 hours"},
    {0x33dd303eu, "The character designs of Ikaruga."},
    {0x8ec1da31u, "To unlock: clear the game (with continues)"},
    {0x14fb8087u, "or play for 10 hours"},
    {0x0d8892bau, "The illustrations of Ikaruga."},
    {0x900c5cb0u, "To unlock: clear the trial game with one credit"},
    {0xd600c3e8u, "or play for 5 hours"},
    {0xfda1ce7bu, "B: Back"},
    {0x5210fff9u, "X: Help on or off"},
    {0x2dc5bcbcu, "A: Play   B: Back"},
    {0xb494962eu, "X: Help on or off   Y: Fade out"},
    // The memory card at the start.
    {0x4394f71bu, "[ Loading the data file ]"},
    {0x659a9b5eu, "[ The data file was loaded ]"},
    {0x41e04453u, "Press START to begin."},
    {0x0e78e288u, "[ The data file could not be loaded ]"},
    {0x64c2c91au, "The data file may be damaged."},
    {0x09d45af5u, "Press START"},
    {0x1709a86au, "to begin with the default settings."},
    {0x2abf0338u, "[ No memory card is connected ]"},
    {0xb29175acu, "To save Ikaruga's data file, a memory card"},
    {0x25a66c4bu, "with at least 34 free blocks"},
    {0xbbcc0d7cu, "must be in expansion socket 1 of port A."},
    {0xfe20f846u, "The game begins without a data file."},
    {0xe096b589u, "[ Creating the data file ]"},
    {0xb0143070u, "[ The data file was created ]"},
    {0x93c55b26u, "[ The data file could not be created ]"},
    {0x9d49547bu, "The memory card may not be connected properly."},
    {0xaed36772u, "[ The memory card has too few free blocks ]"},
};

// The cells of the English line that the game draws now.
std::vector<render::Glyph> cells;

// The coverage of the glyph that FUN_8c06aa40 wrote last, and where, for the copy that follows.
render::Glyph fetched;
uint32_t fetched_at = 0;

uint32_t fnv1a(gptr<byte> text) {
    uint32_t h = 0x811c9dc5u;
    for (uint32_t a = text.a; guest::load<uint8_t>(a) != 0; ++a) h = (h ^ guest::load<uint8_t>(a)) * 0x01000193u;
    return h;
}

}  // namespace

namespace text {

bool glyph(uint16_t code, bool rom, uint32_t at, uint8_t (&bits)[72]) {
    if (code >= kFirstCell && code - kFirstCell < cells.size())
        fetched = cells[code - kFirstCell];
    else if (!rom)
        render::glyph24(code, fetched);
    else
        return false;
    std::copy(fetched.bits.begin(), fetched.bits.end(), bits);
    fetched_at = at;
    return true;
}

}  // namespace text

namespace game {

// The glyph that the port drew, with its coverage as the alpha. The game copies each dot of a ROM
// glyph also onto the dots to its right and below, which widens the ROM's strokes of one dot;
// Hiragino's strokes are as wide already. `pixels` follows the texture's header, whose word 6 holds
// its format: 0x900 ARGB1555, 0x902 ARGB4444, both rectangles.
void FUN_8c06aaa0(gptr<byte> glyph, int pixels, uint x, uint y, ushort stride, undefined2 color) {
    if (glyph.a != fetched_at || fetched_at == 0)
        return FUN_8c06aaa0_game(glyph, pixels, x, y, stride, color);
    fetched_at = 0;
    diff::own_state();
    const uint32_t base = static_cast<uint32_t>(pixels);
    guest::store<uint32_t>(base - 8, (guest::load<uint32_t>(base - 8) & ~0xFFu) | 2);
    for (int row = 0; row < 24; ++row)
        for (int col = 0; col < 24; ++col) {
            if (const int alpha = (fetched.cover[24 * row + col] * 15 + 127) / 255; alpha > 0)
                guest::store<uint16_t>(base + 2 * ((y + row) * stride + x + col), static_cast<uint16_t>(alpha << 12 | 0xFFF));
        }
}

gptr<uint> FUN_8c06ab78(uint align, gptr<byte> text) {
    const auto it = tweaks::current().english ? kEnglish.find(fnv1a(text)) : kEnglish.end();
    if (it == kEnglish.end())
        return FUN_8c06ab78_game(align, text);
    cells = render::line24(it->second);
    cells.resize(std::min(cells.size(), kMaxCells));
    // The line of codes, on the guest's stack.
    const uint32_t bytes = static_cast<uint32_t>(2 * cells.size() + 1);
    guest::Frame frame(bytes);
    const uint32_t line = frame.top - ((bytes + 7u) & ~7u);
    for (uint32_t i = 0; i < cells.size(); ++i) {
        guest::store<uint8_t>(line + 2 * i, static_cast<uint8_t>((kFirstCell + i) >> 8));
        guest::store<uint8_t>(line + 2 * i + 1, static_cast<uint8_t>(kFirstCell + i));
    }
    guest::store<uint8_t>(line + bytes - 1, 0);
    const gptr<uint> texture = FUN_8c06ab78_game(align, gptr<byte>(line));
    cells.clear();
    return texture;
}

}  // namespace game
