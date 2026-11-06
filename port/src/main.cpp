// The native port's entry point: loads the game into guest memory, prepares what the boot ROM would
// have left behind, and runs the decompiled game from its own entry point.
#ifndef _WIN32
#include <execinfo.h>
#include <signal.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "decomp.h"
#include "disc.h"
#include "dream/runtime/aica/aica.h"
#include "dream/runtime/aica/limiter.h"
#include "dream/runtime/maple/maple.h"
#include "dream/runtime/mem/dc_memory.h"
#include "dream/runtime/system.h"
#include "frame.h"
#include "game_tweaks.h"
#include "hw.h"
#include "perf.h"
#include "render/activity.h"
#include "render/audio.h"
#include "render/settings.h"
#include "render/window.h"
#include "saves.h"
#include "sh4/sh4.h"

#ifdef _WIN32
// Included after the decomp: windows.h defines VOID as a macro, which would rewrite the generated
// `typedef void VOID` into `typedef void void`. The game headers do not use windows.h.
#include <windows.h>
#endif

namespace bios {
void install_vectors();
}
namespace cpu {
void report_unknowns(std::FILE* out, size_t limit);
void report_mask_changes(std::FILE* out);
}
namespace guest {
void report_missing_calls(std::FILE* out);
}
namespace trace {
void print(std::FILE* out, size_t last);
void print_stack(std::FILE* out);
void dump(const char* path);
void dump_counts(const char* path);
}
namespace {

struct StopRun {
    std::string why;
};

// Returns the file's size, or 0 when it cannot be read.
size_t load_file(const std::string& path, uint32_t address) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return 0;
    std::vector<char> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    for (size_t i = 0; i < data.size(); ++i)
        guest::store<uint8_t>(address + static_cast<uint32_t>(i), static_cast<uint8_t>(data[i]));
    return data.size();
}

void usage() {
    std::fprintf(stderr,
                 "usage: ikaruga [options]\n"
                 "  --disc FILE.gdi   the disc (default for a window that is not scripted the one it asked for,\n"
                 "                    else game/disc/disc.gdi)\n"
                 "  --boot FILE       1ST_READ.BIN, descrambled (default the disc's)\n"
                 "  --bios FILE       a boot ROM image, for the system font (optional: else macOS's Hiragino)\n"
                 "  --saves DIR       the memory card's files (default saves, or for a window that is not\n"
                 "                    scripted the settings folder's)\n"
                 "  --frames N        stop after N frames\n"
                 "  --window          open a window: picture, sound, keyboard and gamepad, at the\n"
                 "                    console's speed\n"
                 "  --unthrottled     with --window, run as fast as possible\n"
                 "  --fullscreen      with --window, start in fullscreen (the menu's FULLSCREEN row too)\n"
                 "  --tate MODE       with --window, how the picture turns for a monitor stood on its side:\n"
                 "                    auto (a window taller than wide turns it), off, left or right. It turns\n"
                 "                    only while the game is on its VERTICAL SCREEN option\n"
                 "  --scale N         draw at N times 640x480 (default: the menu's GRAPHICS page, 2 at\n"
                 "                    first)\n"
                 "  --screenshot-at LIST write the frames drawn at these frames (300,800) to --screenshot-path\n"
                 "  --screenshot-path FILE (default ikaruga.ppm); with several frames, FILE-N.ppm\n"
                 "  --press LIST      scripted presses: start@600,a@700 holds a button for 8 frames,\n"
                 "                    a@3000-6000 from frame 3000 to 6000; 2start@3000 is on a second\n"
                 "                    controller, which the run then has\n"
                 "  --wav FILE        record the sound as 16-bit stereo at 44.1 kHz\n"
                 "  --no-audio        play no sound in the window\n"
                 "  --no-gamepads     never open a gamepad, for scripted or headless runs\n"
                 "  --record-input FILE  write the controller state of every frame where it changes\n"
                 "  --replay-input FILE  play such a recording back instead of the controls; the format\n"
                 "                    is dream-recomp's, so a recording from the reference build works\n");
}

}  // namespace

// A crash in the decompiled code prints where it happened.
#ifndef _WIN32
void on_crash(int sig) {
    void* frames[64];
    const int n = backtrace(frames, 64);
    trace::print(stderr, 60);
    trace::print_stack(stderr);
    std::fprintf(stderr, "signal %d, backtrace:\n", sig);
    backtrace_symbols_fd(frames, n, STDERR_FILENO);
    _exit(128 + sig);
}
#else
LONG WINAPI on_crash(EXCEPTION_POINTERS* info) {
    trace::print(stderr, 60);
    trace::print_stack(stderr);
    std::fprintf(stderr, "exception %08lx at %p\n", info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0ul,
                 info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionAddress : nullptr);
    std::fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

int main(int argc, char** argv) {
#ifndef _WIN32
    // On its own stack, so that a stack overflow still reports.
    static char alt[1 << 16];
    stack_t ss{};
    ss.ss_sp = alt;
    ss.ss_size = sizeof(alt);
    sigaltstack(&ss, nullptr);
    struct sigaction sa{};
    sa.sa_handler = on_crash;
    sa.sa_flags = SA_ONSTACK;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
#else
    SetUnhandledExceptionFilter(on_crash);
#endif
    std::string disc = "game/disc/disc.gdi", boot, saves_dir;
    bool disc_given = false;
    std::string bios_rom;
    uint64_t max_frames = 0;
    std::vector<uint64_t> screenshots;
    bool window = false, unthrottled = false, audio = true, gamepads = true;
    std::string wav_path;
    int scale = 0, tate = -1;
    bool fullscreen = false;
    std::string screenshot_path = "ikaruga.ppm";
    struct Press {
        uint64_t from, until;
        uint16_t bit;
        unsigned player;  // 1: the controller in port B
    };
    std::vector<Press> presses;
    bool second_presses = false;  // the presses play the controller in port B too
    std::string record_path, replay_path;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "ikaruga: %s needs a value\n", a.c_str());
                usage();
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--disc") {
            disc = next();
            disc_given = true;
        }
        else if (a == "--boot") boot = next();
        else if (a == "--bios") bios_rom = next();
        else if (a == "--saves") saves_dir = next();
        else if (a == "--frames") max_frames = std::strtoull(next().c_str(), nullptr, 0);
        else if (a == "--window") window = true;
        else if (a == "--unthrottled") unthrottled = true;
        else if (a == "--scale") scale = std::atoi(next().c_str());
        else if (a == "--fullscreen") fullscreen = true;
        else if (a == "--tate") {
            const std::string mode = next();
            const char* const names[] = {"auto", "off", "left", "right"};
            tate = -1;
            for (int i = 0; i < 4; ++i)
                if (mode == names[i])
                    tate = i;
            if (tate < 0) {
                std::fprintf(stderr, "ikaruga: --tate takes auto, off, left or right\n");
                return 2;
            }
        }
        else if (a == "--screenshot-at") {
            const std::string list = next();
            for (const char* p = list.c_str(); *p;) {
                char* end;
                screenshots.push_back(std::strtoull(p, &end, 0));
                if (end == p) break;
                p = *end == ',' ? end + 1 : end;
            }
        }
        else if (a == "--screenshot-path") screenshot_path = next();
        else if (a == "--press") {
            const std::string list = next();
            size_t pos = 0;
            while (pos < list.size()) {
                const size_t comma = list.find(',', pos);
                const std::string item = list.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
                const size_t at = item.find('@');
                std::string name = item.substr(0, at);
                unsigned player = 0;
                if (name.size() > 1 && name[0] == '2') {
                    player = 1;
                    name.erase(0, 1);
                }
                char* end = nullptr;
                const uint64_t frame_no = std::strtoull(item.c_str() + at + 1, &end, 0);
                const uint64_t until = *end == '-' ? std::strtoull(end + 1, nullptr, 0) : frame_no + 8;
                const uint16_t bit = name == "start" ? 1 << 3 : name == "a" ? 1 << 2 : name == "b" ? 1 << 1
                                   : name == "x" ? 1 << 10 : name == "y" ? 1 << 9 : name == "up" ? 1 << 4
                                   : name == "down" ? 1 << 5 : name == "left" ? 1 << 6 : 1 << 7;
                presses.push_back({frame_no, until, bit, player});
                if (player)
                    second_presses = hw::second_pad() = true;
                if (comma == std::string::npos) break;
                pos = comma + 1;
            }
        }
        else if (a == "--wav") wav_path = next();
        else if (a == "--record-input") record_path = next();
        else if (a == "--replay-input") replay_path = next();
        else if (a == "--no-audio") audio = false;
        else if (a == "--no-gamepads") gamepads = false;
        else if (a.rfind("-psn_", 0) == 0) continue;  // the process serial number older macOS passes
        else {
            std::fprintf(stderr, "ikaruga: unknown option %s\n", a.c_str());
            usage();
            return 2;
        }
    }

    // The app started from Finder has no options, and plays in a window.
    if (std::strstr(argv[0], ".app/Contents/MacOS/") &&
        std::all_of(argv + 1, argv + argc, [](const char* s) { return std::strncmp(s, "-psn_", 5) == 0; }))
        window = true;
    const bool scripted_run = std::getenv("IKARUGA_AUTOPLAY") || std::getenv("IKARUGA_SHOTS_AFTER") ||
                              !presses.empty() || !replay_path.empty() || max_frames || !screenshots.empty();
    // A run that someone plays, which Finder may have started with no terminal and in /.
    const bool playing = window && !scripted_run;
    auto fail = [&](const std::string& message) {
        std::fprintf(stderr, "%s\n", message.c_str());
        if (playing)
            render::show_error(message);
        return 2;
    };
    namespace fs = std::filesystem;
    std::error_code ec;
    // The disc: --disc, else for a played run the one that disc.txt next to the bindings names,
    // which it asks for once. Other runs take the repository's, or disc.txt's without it.
    bool asked = false;
    if (!disc_given && (playing || !fs::exists(disc, ec))) {
        std::string remembered = render::read_settings(render::settings_path("disc.txt"));
        while (!remembered.empty() && std::isspace(static_cast<unsigned char>(remembered.back())))
            remembered.pop_back();
        if (!remembered.empty() && fs::exists(remembered, ec))
            disc = remembered;
        else if (playing) {
            if (!render::ask_for_disc(disc))
                return 0;
            asked = true;
        }
    }
    hw::system();  // the console's memory is where the files below go
    std::string err;
    if (!hw::open_disc(disc, err))
        return fail("disc: " + err);
    // IP.BIN, with the title that names the disc, and the program, from the disc's file system:
    // a GD-ROM holds 1ST_READ.BIN unscrambled.
    std::vector<uint8_t> ip;
    if (!disc::read_bootstrap(ip) || std::memcmp(ip.data() + 0x80, "IKARUGA ", 8) != 0)
        return fail("disc: " + disc + " is not Ikaruga's");
    if (asked && !render::write_settings(render::settings_path("disc.txt"), disc + "\n"))
        std::fprintf(stderr, "disc: cannot remember %s\n", disc.c_str());
    for (size_t i = 0; i < ip.size(); ++i)
        guest::store<uint8_t>(0x8C008000u + static_cast<uint32_t>(i), ip[i]);
    size_t image = 0;
    if (!boot.empty()) {
        if (!(image = load_file(boot, 0x8C010000u)))
            return fail("cannot read " + boot);
    }
    else {
        std::vector<uint8_t> program;
        if (!disc::read_file("1ST_READ.BIN", program) || program.empty())
            return fail("disc: no 1ST_READ.BIN on " + disc);
        for (size_t i = 0; i < program.size(); ++i)
            guest::store<uint8_t>(0x8C010000u + static_cast<uint32_t>(i), program[i]);
        image = program.size();
    }
    guest::image_end = 0x8C010000u + static_cast<uint32_t>(image);
    // A played run keeps the card and finds the boot ROM next to the bindings. Its first run takes
    // the working directory's card along, which stays where it is.
    if (saves_dir.empty() && playing) {
        saves_dir = render::settings_path("saves");
        if (!fs::exists(saves_dir, ec) && fs::is_directory("saves", ec)) {
            fs::copy("saves", saves_dir, fs::copy_options::recursive, ec);
            std::printf("card: %s %s from saves\n", ec ? "cannot copy" : "copied", saves_dir.c_str());
        }
    }
    if (!saves_dir.empty())
        saves::set_directory(saves_dir);
    if (bios_rom.empty() && playing)
        for (const std::string& p : {render::settings_path("dc_bios.bin"), std::string("bios/dc_bios.bin")})
            if (fs::exists(p, ec)) {
                bios_rom = p;
                break;
            }
    if (!bios_rom.empty() && !hw::load_rom(bios_rom))
        std::fprintf(stderr, "cannot read %s; the system font will be empty\n", bios_rom.c_str());
    bios::install_vectors();
    // The boot ROM hands over with the stack at the top of the game's stack area, which the startup
    // code sets again from its own literal pool.
    guest::sp = 0x8C00F000u;
    if (std::getenv("IKARUGA_SH4_TEST"))
        return sh4::self_test() ? 1 : 0;

    auto is_screenshot = [&](uint64_t n) {
        return std::find(screenshots.begin(), screenshots.end(), n) != screenshots.end();
    };
    auto suffixed = [&](const std::string& suffix) {
        const size_t dot = screenshot_path.rfind('.');
        const std::string stem = dot == std::string::npos ? screenshot_path : screenshot_path.substr(0, dot);
        const std::string ext = dot == std::string::npos ? "" : screenshot_path.substr(dot);
        return stem + suffix + ext;
    };
    auto shot_path = [&](uint64_t n) {
        return screenshots.size() == 1 ? screenshot_path : suffixed("-" + std::to_string(n));
    };

    // The picture: the renderer draws each display list when the game starts the render, and the
    // window shows what the console would scan out at each vertical blank.
    std::unique_ptr<render::Window> screen;
    auto hardware = [] {
        return render::Hardware{hw::pvr_registers(), hw::system().memory.vram(), dream::mem::DcMemory::kVramSize};
    };
    if (window) {
        std::string error;
        // Metal shows its performance HUD only when this is set before the first device; the menu's
        // METAL HUD then shows or hides it (render/gpu.h). Not in a scripted run, whose output it
        // would fill with its notes.
        if (!scripted_run) {
#ifdef __APPLE__
            setenv("MTL_HUD_ENABLED", "1", 0);
            setenv("MTL_HUD_DISABLE_MENU_BAR", "1", 0);  // the menu's row switches it, not the menu bar's
#endif
            hw::second_pad() = true;
        }
        screen = std::make_unique<render::Window>();
        screen->start_fullscreen(fullscreen);
        screen->start_tate(tate);
        // screenHorizontal: 0 while the game is on its vertical screen (docs/game.md).
        screen->vertical_screen = [] { return guest::load<uint8_t>(0x8C1CB150u) == 0; };
        if (!screen->open(static_cast<unsigned>(scale), "Ikaruga", scripted_run, gamepads, error)) {
            std::fprintf(stderr, "window: %s\n", error.c_str());
            return 2;
        }
        render::keep_running(unthrottled);
        hw::on_render([&screen, &hardware](const std::vector<uint32_t>& stream) { screen->render(stream, hardware()); });
    }

    // Sound: the AICA's samples go to the audio device, and to a WAV file if asked.
    render::Audio sink;
    if (window && audio && !sink.open())
        std::fprintf(stderr, "audio: %s\n", sink.error().c_str());
    std::FILE* wav = wav_path.empty() ? nullptr : std::fopen(wav_path.c_str(), "wb");
    uint32_t wav_bytes = 0;
    auto wav_header = [&] {
        const uint32_t rate = 44100, byte_rate = rate * 4, riff = 36 + wav_bytes;
        const uint16_t fmt = 1, channels = 2, align = 4, bits = 16;
        const uint32_t fmt_size = 16;
        std::fseek(wav, 0, SEEK_SET);
        std::fwrite("RIFF", 1, 4, wav);
        std::fwrite(&riff, 4, 1, wav);
        std::fwrite("WAVEfmt ", 1, 8, wav);
        std::fwrite(&fmt_size, 4, 1, wav);
        std::fwrite(&fmt, 2, 1, wav);
        std::fwrite(&channels, 2, 1, wav);
        std::fwrite(&rate, 4, 1, wav);
        std::fwrite(&byte_rate, 4, 1, wav);
        std::fwrite(&align, 2, 1, wav);
        std::fwrite(&bits, 2, 1, wav);
        std::fwrite("data", 1, 4, wav);
        std::fwrite(&wav_bytes, 4, 1, wav);
    };
    if (wav)
        wav_header();
    // A hash of every sample pair, for the summary and IKARUGA_STREAMS: two builds that play the
    // same game make the same sound.
    uint64_t sound_hash = 0xCBF29CE484222325ull;
    // At full master volume, Ikaruga's mix reaches twice the 16-bit range in busy scenes, and the
    // clipped peaks distort the explosions. Half the level fits all but the largest explosions,
    // whose peaks reach four times the range, and the limiter brings those under it.
    hw::aica().mixer.headroom = 1;
    static dream::aica::Limiter limiter;
    hw::aica().mixer.before_clamp = [](int32_t& l, int32_t& r) { limiter.process(l, r); };
    hw::aica().on_sample = [&](int16_t l, int16_t r) {
        sound_hash = (sound_hash ^ (uint32_t{static_cast<uint16_t>(l)} | uint32_t{static_cast<uint16_t>(r)} << 16)) *
                     0x100000001B3ull;
        if (sink.is_open())
            sink.push(l, r);
        if (wav) {
            const int16_t pair[2] = {l, r};
            std::fwrite(pair, 2, 2, wav);
            wav_bytes += 4;
        }
    };

    // A recording: one line per frame where the controller changed, "frame buttons rtrigger ltrigger
    // joy_x joy_y joy2_x joy2_y", buttons in hex and active low (dream-recomp's --record-input).
    using Pad = dream::maple::ControllerState;
    std::vector<std::pair<uint64_t, Pad>> replayed;
    size_t replay_next = 0;
    if (!replay_path.empty()) {
        std::ifstream rec(replay_path);
        if (!rec) {
            std::fprintf(stderr, "--replay-input %s: cannot read it\n", replay_path.c_str());
            return 2;
        }
        std::string line;
        while (std::getline(rec, line)) {
            unsigned long long f = 0;
            unsigned b = 0, rt = 0, lt = 0, x = 0, y = 0, x2 = 0, y2 = 0;
            if (line.empty() || line[0] == '#' ||
                std::sscanf(line.c_str(), "%llu %x %u %u %u %u %u %u", &f, &b, &rt, &lt, &x, &y, &x2, &y2) != 8)
                continue;
            Pad p;
            p.buttons = static_cast<uint16_t>(b);
            p.rtrigger = static_cast<uint8_t>(rt);
            p.ltrigger = static_cast<uint8_t>(lt);
            p.joy_x = static_cast<uint8_t>(x);
            p.joy_y = static_cast<uint8_t>(y);
            p.joy2_x = static_cast<uint8_t>(x2);
            p.joy2_y = static_cast<uint8_t>(y2);
            replayed.push_back({f, p});
        }
        std::printf("input: replaying %zu controller changes from %s\n", replayed.size(), replay_path.c_str());
    }
    std::FILE* record = nullptr;
    Pad last_recorded;
    bool recorded_any = false;
    if (!record_path.empty()) {
        record = std::fopen(record_path.c_str(), "w");
        if (!record) {
            std::fprintf(stderr, "--record-input %s: cannot write it\n", record_path.c_str());
            return 2;
        }
        std::fprintf(record, "# frame buttons rtrigger ltrigger joy_x joy_y joy2_x joy2_y\n");
    }

    // At each vertical blank: the recording, the scripted presses or the controls, the picture, the
    // pace and the frame limit. The guest clock is the reference: the window sleeps only while ahead
    // of it.
    const auto started = std::chrono::steady_clock::now();
    auto paused_for = std::chrono::steady_clock::duration::zero();
    double last_vblank = 0;  // the guest's seconds at the last vertical blank
    // IKARUGA_PEEK=ADDR[:SIZE][,ADDR[:SIZE]...]: prints the value at each address when it changes, at
    // the vertical blank. SIZE is 1 (the default), 2 or 4; a 4-byte value is printed as a float too.
    struct Peek {
        uint32_t address;
        unsigned size;
        int64_t last;
    };
    std::vector<Peek> peeks;
    if (const char* v = std::getenv("IKARUGA_PEEK"))
        for (const char* p = v; *p;) {
            char* end = nullptr;
            const auto address = static_cast<uint32_t>(std::strtoul(p, &end, 0));
            if (end == p)
                break;
            unsigned size = 1;
            if (*end == ':')
                size = static_cast<unsigned>(std::strtoul(end + 1, &end, 0));
            peeks.push_back({address, size == 2 || size == 4 ? size : 1, -1});
            p = *end == ',' ? end + 1 : end;
        }
    // IKARUGA_DUMP=FRAME[,FRAME...],DIR: writes main RAM at these vertical blanks, as DIR/ram-FRAME.bin.
    std::vector<uint64_t> dump_frames;
    std::string dump_dir;
    if (const char* v = std::getenv("IKARUGA_DUMP"))
        for (const char* p = v; *p;) {
            char* end = nullptr;
            const uint64_t n = std::strtoull(p, &end, 0);
            if (end == p || (*end != ',' && *end)) {
                dump_dir = p;
                break;
            }
            dump_frames.push_back(n);
            p = *end == ',' ? end + 1 : end;
        }
    // IKARUGA_DUMP_TA=FRAME,FILE: writes the first display list from that frame on as FILE, with
    // video memory and the PVR registers as FILE.vram and FILE.vram.regs: the files the reference
    // build's --dump-ta writes, for dream_render_view.
    if (const char* v = std::getenv("IKARUGA_DUMP_TA")) {
        char* end = nullptr;
        const uint64_t from = std::strtoull(v, &end, 0);
        const std::string path = *end == ',' ? end + 1 : "ikaruga.ta";
        hw::on_render([from, path, done = false](const std::vector<uint32_t>& stream) mutable {
            if (done || hw::frames() < from)
                return;
            done = true;
            if (std::FILE* f = std::fopen(path.c_str(), "wb")) {
                std::fwrite(stream.data(), 4, stream.size(), f);
                std::fclose(f);
            }
            if (std::FILE* f = std::fopen((path + ".vram").c_str(), "wb")) {
                std::fwrite(guest::vram(), 1, hw::kVramSize, f);
                std::fclose(f);
            }
            if (std::FILE* f = std::fopen((path + ".vram.regs").c_str(), "wb")) {
                for (uint32_t off = 0; off < 0x2000; off += 4) {
                    const uint32_t word = hw::pvr_registers()[off / 4];
                    std::fwrite(&word, 4, 1, f);
                }
                std::fclose(f);
            }
            std::printf("wrote %s: the display list of frame %llu, %zu words\n", path.c_str(),
                        static_cast<unsigned long long>(hw::frames()), stream.size());
        });
    }
    // IKARUGA_STREAMS=FILE: one line for each render: its number, the frame, the display list's
    // words, and hashes of the display list, of the registers the renderer reads, of video memory,
    // of the decoded display list, which leaves out the words that the renderer ignores, and of the
    // sound so far. Two builds that play the same game give the same lines. A change that moves the
    // timing changes them: the game's animations read the time.
    std::FILE* streams = nullptr;
    if (const char* path = std::getenv("IKARUGA_STREAMS")) {
        streams = std::fopen(path, "w");
        if (!streams) {
            std::fprintf(stderr, "IKARUGA_STREAMS: cannot write %s\n", path);
            return 2;
        }
        hw::on_render([streams, &hardware, &sound_hash, n = uint64_t{0}](const std::vector<uint32_t>& stream) mutable {
            const auto hash = [](const void* data, size_t bytes, uint64_t h = 0xCBF29CE484222325ull) {
                const auto* p = static_cast<const uint8_t*>(data);
                for (size_t i = 0; i + 4 <= bytes; i += 4) {
                    uint32_t w;
                    std::memcpy(&w, p + i, 4);
                    h = (h ^ w) * 0x100000001B3ull;
                }
                return h;
            };
            const render::Hardware hw = hardware();
            // PARAM_BASE, FB_W_SOF1, the clip, the background, fog, TEXT_CONTROL, PAL_RAM_CTRL,
            // PT_ALPHA_REF, then the fog table and the palette.
            static constexpr uint32_t kRegisters[] = {0x020, 0x060, 0x068, 0x06C, 0x088, 0x08C, 0x0B0,
                                                      0x0B4, 0x0B8, 0x0E4, 0x108, 0x11C};
            uint64_t regs = 0xCBF29CE484222325ull;
            for (uint32_t off : kRegisters) regs = hash(&hw.registers[off / 4], 4, regs ^ off);
            regs = hash(&hw.registers[0x200 / 4], 0x200, regs);
            regs = hash(&hw.registers[0x1000 / 4], 0x1000, regs);
            static render::TaDecoder decoder;
            decoder.reset();
            decoder.feed(stream.data(), stream.size());
            std::fprintf(streams, "%llu %llu %zu %016llx %016llx %016llx %016llx %016llx\n",
                         static_cast<unsigned long long>(n++), static_cast<unsigned long long>(hw::frames()),
                         stream.size(), static_cast<unsigned long long>(hash(stream.data(), stream.size() * 4)),
                         static_cast<unsigned long long>(regs),
                         static_cast<unsigned long long>(hash(hw.vram, hw.vram_size)),
                         static_cast<unsigned long long>(frame::hash(decoder.list())),
                         static_cast<unsigned long long>(sound_hash));
        });
    }
    // IKARUGA_POKE=ADDR=BYTE[,ADDR=BYTE...]: writes these bytes at each vertical blank, e.g. to keep
    // the lives counter up while a test plays to a later part of a stage.
    std::vector<std::pair<uint32_t, uint8_t>> pokes;
    if (const char* v = std::getenv("IKARUGA_POKE"))
        for (const char* p = v; *p;) {
            char* end = nullptr;
            const uint32_t address = static_cast<uint32_t>(std::strtoul(p, &end, 0));
            if (end == p || *end != '=')
                break;
            p = end + 1;
            pokes.push_back({address, static_cast<uint8_t>(std::strtoul(p, &end, 0))});
            if (end == p)
                break;
            p = *end == ',' ? end + 1 : end;
        }
    // IKARUGA_SHOTS_AFTER=ADDR+N[,ADDR+N...]: a screenshot N frames after the first call of the
    // function at ADDR, named after the --screenshot-path as STEM-ADDR+N.ppm. The pictures follow the
    // game's events, so a change that moves the timing still shows the same scenes.
    struct ShotAfter {
        uint32_t address;
        uint64_t frames;
    };
    std::vector<ShotAfter> shots_after;
    if (const char* v = std::getenv("IKARUGA_SHOTS_AFTER"))
        for (const char* p = v; *p;) {
            char* end = nullptr;
            const auto address = static_cast<uint32_t>(std::strtoul(p, &end, 0));
            if (end == p || *end != '+')
                break;
            p = end + 1;
            shots_after.push_back({address, std::strtoull(p, &end, 0)});
            diff::watch(address);
            p = *end == ',' ? end + 1 : end;
        }
    // IKARUGA_AUTOPLAY: plays without a recording, for checks that must not depend on the game's
    // timing (tools/port/check.py). Start and A in turns get through the menus with their defaults.
    // In a stage (scenes 6 to 10, at 0x8C15C560) Start would pause, so nothing is pressed until the
    // stage scrolls (its speed at 0x8C1D142C). From then on A stays held, which shoots, and the
    // lives counter at 0x8C1D13EC stays at 2. Autosave (0x8C1CD76A, the options menu's setting)
    // goes on at that point, so the game saves to the memory card when it returns to the title
    // after the ending.
    const bool autoplay = std::getenv("IKARUGA_AUTOPLAY") != nullptr;
    bool autoplay_in_stage = false;
    const render::Tweaks env_tweaks = tweaks::from_environment();
    // IKARUGA_PERF: the time from the end of the last frame hook to the start of this one is the game's
    // own code and its render, which the counters in perf.h split. A frame that took longer than
    // 10 ms there is printed, with the hook's own time (the picture, the pacing's sleep excluded).
    perf::Clock::time_point perf_hook_end = perf::Clock::now();
    uint64_t perf_sectors = 0;
    hw::on_vblank([&] {
        const uint64_t frame = hw::frames();
        const perf::Clock::time_point perf_hook_start = perf::Clock::now();
        if (perf::on()) {
            const double guest_ms = std::chrono::duration<double, std::milli>(perf_hook_start - perf_hook_end).count();
            perf::Frame& f = perf::frame();
            const uint64_t sectors = hw::stats().gd_sectors - perf_sectors;
            perf_sectors = hw::stats().gd_sectors;
            if (guest_ms > 10.0)
                std::fprintf(stderr,
                             "perf: frame %llu took %.1f ms: render %.1f (decode %.1f, upload %.1f, hash %.1f; "
                             "%llu textures, %llu uploads, %.1f MB; %llu pipelines %.1f ms), other %.1f, %llu disc sectors\n",
                             static_cast<unsigned long long>(frame), guest_ms, f.render_ms, f.decode_ms, f.upload_ms,
                             f.hash_ms, static_cast<unsigned long long>(f.decoded),
                             static_cast<unsigned long long>(f.uploads), static_cast<double>(f.bytes) / 1048576.0,
                             static_cast<unsigned long long>(f.pipelines), f.pipeline_ms, guest_ms - f.render_ms, static_cast<unsigned long long>(sectors));
            f = perf::Frame{};
        }
        struct HookEnd {
            perf::Clock::time_point& at;
            ~HookEnd() { at = perf::Clock::now(); }
        } hook_end{perf_hook_end};
        if (!dump_dir.empty() && std::find(dump_frames.begin(), dump_frames.end(), frame) != dump_frames.end()) {
            const std::string path = dump_dir + "/ram-" + std::to_string(frame) + ".bin";
            if (std::FILE* f = std::fopen(path.c_str(), "wb")) {
                std::fwrite(guest::ram, 1, guest::kRamSize, f);
                std::fclose(f);
            }
        }
        for (const auto& [address, value] : pokes)
            guest::store<uint8_t>(address, value);
        const render::Tweaks on = (screen ? screen->tweaks() : render::Tweaks{}).either(env_tweaks);
        tweaks::apply(on);
        if (screen)
            screen->gpu().set_english(on.english);
        for (Peek& k : peeks) {
            const int64_t now = k.size == 4   ? guest::load<uint32_t>(k.address)
                                : k.size == 2 ? guest::load<uint16_t>(k.address)
                                              : guest::load<uint8_t>(k.address);
            if (now != k.last) {
                std::printf("peek frame %llu: 0x%08x = 0x%0*llx", static_cast<unsigned long long>(frame), k.address,
                            static_cast<int>(k.size * 2), static_cast<unsigned long long>(now));
                if (k.size == 4)
                    std::printf(" (%.9g)", static_cast<double>(pun<float>(static_cast<uint32_t>(now))));
                std::printf("\n");
            }
            k.last = now;
        }
        dream::maple::ControllerState& pad = hw::pad();
        uint16_t scripted = 0, scripted_second = 0;
        for (const Press& p : presses)
            if (frame >= p.from && frame < p.until)
                (p.player ? scripted_second : scripted) |= p.bit;
        if (autoplay) {
            constexpr uint16_t kStart = 1 << 3, kA = 1 << 2;
            if (!autoplay_in_stage && guest::load<float>(0x8C1D142Cu) != 0.0f) {
                autoplay_in_stage = true;
                guest::store<uint8_t>(0x8C1CD76Au, 1);
            }
            if (autoplay_in_stage) {
                scripted |= kA;
                guest::store<uint8_t>(0x8C1D13ECu, 2);
            }
            else if (const uint8_t scene = guest::load<uint8_t>(0x8C15C560u); (scene < 6 || scene > 10) && frame % 60 < 8)
                scripted |= frame % 120 < 60 ? kStart : kA;
        }
        auto screenshot = [&](const std::string& path) {
            if (!screen->screenshot(hardware(), path))
                std::fprintf(stderr, "screenshot: no rendered frame on screen\n");
        };
        if (screen) {
            const auto guest_seconds = [] { return static_cast<double>(hw::now()) / hw::kCpuHz; };
            if (!screen->present(hardware(), guest_seconds()))
                throw StopRun{"the window was closed"};
            if (screen->menu_open()) {
                const auto paused_at = std::chrono::steady_clock::now();
                while (screen->menu_open()) {
                    if (!screen->present(hardware(), guest_seconds()))
                        throw StopRun{"the window was closed"};
                    std::this_thread::sleep_for(std::chrono::milliseconds(8));
                }
                paused_for += std::chrono::steady_clock::now() - paused_at;
            }
        }
        if (screen) {
            if (is_screenshot(frame)) {
                const std::string path = shot_path(frame);
                screenshot(path);
                std::printf("screenshot of frame %llu: %s\n", static_cast<unsigned long long>(frame), path.c_str());
            }
            for (const ShotAfter& sa : shots_after) {
                const uint64_t first = diff::first_call(sa.address);
                if (first == diff::kNotYet || frame != first + sa.frames)
                    continue;
                char name[64];
                std::snprintf(name, sizeof name, "-%08x+%llu", sa.address, static_cast<unsigned long long>(sa.frames));
                const std::string path = suffixed(name);
                screenshot(path);
                std::printf("screenshot of frame %llu: %s\n", static_cast<unsigned long long>(frame), path.c_str());
            }
            if (!unthrottled) {
                const double seconds = static_cast<double>(hw::now()) / hw::kCpuHz;
                const auto guest = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>(seconds));
                const double frame_seconds = seconds - last_vblank;
                last_vblank = seconds;
                const auto target = started + paused_for + guest;
                const auto now = std::chrono::steady_clock::now();
                // After a stall of more than two frames, the run catches up by the guest's clock, so
                // that the sound's queue fills again, and the display paces it again from there.
                const bool behind = now - target > std::chrono::duration<double>(2 * frame_seconds);
                if (screen->pace(frame_seconds, behind)) {
                    // The display paces the run, and the guest's clock goes on from where it is.
                    paused_for = std::chrono::steady_clock::now() - started - guest;
                }
                else if (target > now && target - now < std::chrono::seconds(1))
                    std::this_thread::sleep_for(target - now);
                sink.set_speed(screen->speed());
            }
            // The controls after the pace's sleep, so that the frame reads them as late as it can.
            if (replayed.empty()) {
                screen->read_input();
                auto take = [&](dream::maple::ControllerState& p, unsigned player) {
                    const render::Controls c = screen->controls(player);
                    p.buttons = static_cast<uint16_t>(0xFFFF & ~c.held);
                    p.joy_x = c.stick_x;
                    p.joy_y = c.stick_y;
                    p.ltrigger = c.left;
                    p.rtrigger = c.right;
                };
                take(pad, 0);
                take(hw::pad(1), 1);
                float dx = 0, dy = 0;
                uint16_t mouse_buttons = 0;
                if (screen->mouse(dx, dy, mouse_buttons)) {
                    pad.buttons = static_cast<uint16_t>(pad.buttons & ~mouse_buttons);
                    tweaks::ship_by(dx, dy);
                }
            }
        }
        // Scripted presses add to what the player holds; without a window nobody holds anything.
        if (!replayed.empty()) {
            while (replay_next < replayed.size() && replayed[replay_next].first <= frame)
                pad = replayed[replay_next++].second;
        }
        else if (!screen)
            pad.buttons = 0xFFFF;
        pad.buttons = static_cast<uint16_t>(pad.buttons & ~scripted);
        if (second_presses)
            hw::pad(1).buttons = static_cast<uint16_t>(0xFFFF & ~scripted_second);
        if (record) {
            const Pad& p = pad;
            if (!recorded_any || p.buttons != last_recorded.buttons || p.rtrigger != last_recorded.rtrigger ||
                p.ltrigger != last_recorded.ltrigger || p.joy_x != last_recorded.joy_x ||
                p.joy_y != last_recorded.joy_y || p.joy2_x != last_recorded.joy2_x ||
                p.joy2_y != last_recorded.joy2_y) {
                std::fprintf(record, "%llu %04x %u %u %u %u %u %u\n", static_cast<unsigned long long>(frame),
                             p.buttons, p.rtrigger, p.ltrigger, p.joy_x, p.joy_y, p.joy2_x, p.joy2_y);
                std::fflush(record);
                last_recorded = p;
                recorded_any = true;
            }
        }
        if (max_frames && frame >= max_frames) {
            trace::print_stack(stdout);
            sh4::report(stdout);
#ifndef _WIN32
            if (std::getenv("IKARUGA_BACKTRACE")) {
                void* frames[64];
                const int n = backtrace(frames, 64);
                std::fflush(stdout);
                backtrace_symbols_fd(frames, n, STDOUT_FILENO);
            }
#endif
            throw StopRun{"frame limit"};
        }
    });

    std::string why = "returned";
    try {
        // The entry through the dispatch, with the registers the boot ROM leaves: nothing in them.
        guest::call(0x8C010000u, CallArgs{});
    }
    catch (const StopRun& s) {
        why = s.why;
    }
    catch (const game::ExitRequested&) {
        why = "the game exited";
        trace::print(stdout, 120);
    }

    if (const char* path = std::getenv("IKARUGA_TRACE_FILE"))
        trace::dump(path);
    if (const char* path = std::getenv("IKARUGA_COUNT_FILE"))
        trace::dump_counts(path);
    if (wav) {
        wav_header();
        std::fclose(wav);
    }
    if (streams)
        std::fclose(streams);
    if (sink.is_open())
        std::printf("sound: %llu samples, %llu dropped because the run was ahead of the device, %llu refills with "
                    "silence after the device ran dry; the queue held %d to %d, the rate ratio ended at %.5f\n",
                    static_cast<unsigned long long>(sink.pushed), static_cast<unsigned long long>(sink.dropped),
                    static_cast<unsigned long long>(sink.refills),
                    sink.least, sink.most, static_cast<double>(sink.ratio()));
    const hw::Stats& s = hw::stats();
    std::printf("stop: %s after %llu frames\n", why.c_str(), static_cast<unsigned long long>(hw::frames()));
    std::printf("renders %llu, store-queue flushes %llu, interrupts %llu, video events %llu, GD-ROM sectors %llu, "
                "sound samples %llu, sound hash %016llx\n",
                static_cast<unsigned long long>(hw::renders()), static_cast<unsigned long long>(s.sq_flushes),
                static_cast<unsigned long long>(s.interrupts), static_cast<unsigned long long>(s.video_events),
                static_cast<unsigned long long>(s.gd_sectors), static_cast<unsigned long long>(hw::aica().samples),
                static_cast<unsigned long long>(sound_hash));
    hw::report_interrupts(stdout);
    sh4::report_profile(stdout);
    cpu::report_mask_changes(stdout);
    hw::report_unhandled(stdout, 40);
    // IKARUGA_UNKNOWNS=N lists N of them instead of 30.
    const char* unknowns = std::getenv("IKARUGA_UNKNOWNS");
    cpu::report_unknowns(stdout, unknowns ? std::strtoul(unknowns, nullptr, 0) : 30);
    guest::report_missing_calls(stdout);
    diff::report(stdout);
    diff::write_called();
    hw::write_users();
    return 0;
}
