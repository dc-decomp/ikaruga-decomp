// The graphical launcher for Linux and Windows. It loads a GDI or CHD, prepares it for the game,
// and starts ikaruga with the chosen options. It uses SDL3's renderer and the vendored ImGui
// backends, so the launcher itself needs no desktop toolkit. Command-line invocations still go
// through launcher.cpp's CLI path.
#include "launcher.h"

#include <SDL3/SDL.h>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
using PathText = std::array<char, 2048>;

constexpr ImVec4 kAccent{0.88f, 0.22f, 0.20f, 1.0f};
constexpr ImVec4 kMuted{0.57f, 0.60f, 0.66f, 1.0f};
constexpr ImVec4 kError{0.96f, 0.38f, 0.38f, 1.0f};
constexpr ImVec4 kReady{0.46f, 0.80f, 0.52f, 1.0f};

enum class State { Idle, Preparing, Running, Done, Failed };

enum class BrowseTarget {
    None,
    Disc,
    Data,
    Prepared,
    Saves,
    Bios,
    Boot,
    Wav,
    Screenshot,
    Record,
    Replay,
};

struct Gui {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;

    PathText disc{};
    PathText data{};
    PathText prepared{};
    PathText saves{};
    PathText bios{};
    PathText boot{};
    PathText wav{};
    PathText screenshot{};
    PathText screenshot_at{};
    PathText press{};
    PathText record{};
    PathText replay{};
    PathText extra{};

    int scale = 0;
    int frames = 0;
    int tate = 0;  // 0 the game's own setting, else 1 auto, 2 off, 3 left, 4 right (--tate)
    bool fullscreen = false;
    bool unthrottled = false;
    bool audio = true;
    bool gamepads = true;
    bool extract = true;
    bool rebuild = false;

    fs::path settings_file;
    std::vector<fs::path> detected;

    std::atomic<State> state{State::Idle};
    std::mutex state_mutex;
    std::string status = "Ready.";
    std::string error;
    std::thread worker;

    bool minimized = false;
    bool quit_requested = false;

    std::mutex dialog_mutex;
    bool dialog_open = false;
    bool dialog_done = false;
    BrowseTarget dialog_target = BrowseTarget::None;
    std::string dialog_path;
    std::string dialog_error;
    std::string dialog_default;
};

void copy_path(PathText& text, const fs::path& path) {
    const std::string value = path.string();
    std::snprintf(text.data(), text.size(), "%s", value.c_str());
}

fs::path path_of(const PathText& text) { return fs::path(text.data()); }

bool busy(const Gui& gui) {
    const State state = gui.state.load();
    return state == State::Preparing || state == State::Running;
}

fs::path base_dir() {
    const char* path = SDL_GetBasePath();
    return path ? fs::path(path) : fs::path{};
}

fs::path pref_dir() {
    char* path = SDL_GetPrefPath("ikaruga", "ikaruga");
    if (!path)
        return {};
    const fs::path result(path);
    SDL_free(path);
    return result;
}

fs::path default_data_dir() {
    const fs::path base = base_dir();
    std::error_code ec;
    if (!base.empty() && fs::exists(base.parent_path() / "game", ec))
        return base.parent_path();
    const fs::path pref = pref_dir();
    return pref.empty() ? base : pref;
}

void set_message(Gui& gui, const std::string& message) {
    std::lock_guard<std::mutex> lock(gui.state_mutex);
    gui.status = message;
    gui.error.clear();
}

void set_state(Gui& gui, State state, std::string message, std::string error = {}) {
    {
        std::lock_guard<std::mutex> lock(gui.state_mutex);
        gui.status = std::move(message);
        gui.error = std::move(error);
    }
    gui.state.store(state);
}

std::pair<std::string, std::string> state_text(Gui& gui) {
    std::lock_guard<std::mutex> lock(gui.state_mutex);
    return {gui.status, gui.error};
}

void set_path_field(Gui& gui, BrowseTarget target, const fs::path& path) {
    switch (target) {
        case BrowseTarget::Disc: copy_path(gui.disc, path); break;
        case BrowseTarget::Data: copy_path(gui.data, path); break;
        case BrowseTarget::Prepared: copy_path(gui.prepared, path); break;
        case BrowseTarget::Saves: copy_path(gui.saves, path); break;
        case BrowseTarget::Bios: copy_path(gui.bios, path); break;
        case BrowseTarget::Boot: copy_path(gui.boot, path); break;
        case BrowseTarget::Wav: copy_path(gui.wav, path); break;
        case BrowseTarget::Screenshot: copy_path(gui.screenshot, path); break;
        case BrowseTarget::Record: copy_path(gui.record, path); break;
        case BrowseTarget::Replay: copy_path(gui.replay, path); break;
        case BrowseTarget::None: break;
    }
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

std::string disc_kind(const fs::path& disc) {
    const std::string ext = lower(disc.extension().string());
    if (ext == ".chd")
        return "CHD";
    if (ext == ".gdi")
        return "GDI";
    return {};
}

std::string disc_kind(const PathText& disc) { return disc_kind(fs::path(disc.data())); }

void discover_discs(Gui& gui) {
    gui.detected.clear();
    const fs::path base = base_dir();
    const fs::path data = path_of(gui.data);
    std::vector<fs::path> dirs = {base.parent_path() / "ROM", data / "ROM", data / "game" / "disc",
                                  data / "game" / "prepared"};
    std::error_code ec;
    for (const fs::path& dir : dirs) {
        if (!fs::is_directory(dir, ec))
            continue;
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file(ec))
                continue;
            const std::string ext = lower(entry.path().extension().string());
            if (ext != ".gdi" && ext != ".chd")
                continue;
            const fs::path path = fs::absolute(entry.path(), ec);
            if (std::find(gui.detected.begin(), gui.detected.end(), path) == gui.detected.end())
                gui.detected.push_back(path);
        }
    }
    std::sort(gui.detected.begin(), gui.detected.end());
    if (gui.disc[0] == '\0' && gui.detected.size() == 1)
        copy_path(gui.disc, gui.detected.front());
}

void load_settings(Gui& gui) {
    if (gui.settings_file.empty())
        return;
    std::ifstream in(gui.settings_file);
    if (!in)
        return;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        const size_t equals = line.find('=');
        if (equals == std::string::npos)
            continue;
        const std::string key = line.substr(0, equals);
        const std::string value = line.substr(equals + 1);
        auto set = [&](PathText& field) { std::snprintf(field.data(), field.size(), "%s", value.c_str()); };
        if (key == "disc") set(gui.disc);
        else if (key == "data") set(gui.data);
        else if (key == "prepared") set(gui.prepared);
        else if (key == "saves") set(gui.saves);
        else if (key == "bios") set(gui.bios);
        else if (key == "boot") set(gui.boot);
        else if (key == "wav") set(gui.wav);
        else if (key == "screenshot") set(gui.screenshot);
        else if (key == "screenshot_at") set(gui.screenshot_at);
        else if (key == "press") set(gui.press);
        else if (key == "record") set(gui.record);
        else if (key == "replay") set(gui.replay);
        else if (key == "extra") set(gui.extra);
        else if (key == "scale") gui.scale = std::max(0, std::min(6, std::atoi(value.c_str())));
        else if (key == "frames") gui.frames = std::max(0, std::atoi(value.c_str()));
        else if (key == "fullscreen") gui.fullscreen = value == "1";
        else if (key == "tate") gui.tate = std::max(0, std::min(4, std::atoi(value.c_str())));
        else if (key == "unthrottled") gui.unthrottled = value == "1";
        else if (key == "audio") gui.audio = value == "1";
        else if (key == "gamepads") gui.gamepads = value == "1";
        else if (key == "extract") gui.extract = value == "1";
        else if (key == "rebuild") gui.rebuild = value == "1";
    }
}

void save_settings(const Gui& gui) {
    if (gui.settings_file.empty())
        return;
    std::error_code ec;
    if (!gui.settings_file.parent_path().empty())
        fs::create_directories(gui.settings_file.parent_path(), ec);
    std::ofstream out(gui.settings_file);
    if (!out)
        return;
    auto write = [&](const char* key, const PathText& value) { out << key << '=' << value.data() << '\n'; };
    write("disc", gui.disc);
    write("data", gui.data);
    write("prepared", gui.prepared);
    write("saves", gui.saves);
    write("bios", gui.bios);
    write("boot", gui.boot);
    write("wav", gui.wav);
    write("screenshot", gui.screenshot);
    write("screenshot_at", gui.screenshot_at);
    write("press", gui.press);
    write("record", gui.record);
    write("replay", gui.replay);
    write("extra", gui.extra);
    out << "scale=" << gui.scale << '\n'
        << "frames=" << gui.frames << '\n'
        << "fullscreen=" << (gui.fullscreen ? 1 : 0) << '\n'
        << "tate=" << gui.tate << '\n'
        << "unthrottled=" << (gui.unthrottled ? 1 : 0) << '\n'
        << "audio=" << (gui.audio ? 1 : 0) << '\n'
        << "gamepads=" << (gui.gamepads ? 1 : 0) << '\n'
        << "extract=" << (gui.extract ? 1 : 0) << '\n'
        << "rebuild=" << (gui.rebuild ? 1 : 0) << '\n';
}

void SDLCALL dialog_callback(void* userdata, const char* const* files, int) {
    auto* gui = static_cast<Gui*>(userdata);
    std::lock_guard<std::mutex> lock(gui->dialog_mutex);
    gui->dialog_done = true;
    if (!files) {
        gui->dialog_error = SDL_GetError();
        if (gui->dialog_error.empty())
            gui->dialog_error = "the file dialog failed";
        return;
    }
    if (files[0])
        gui->dialog_path = files[0];
}

void begin_browse(Gui& gui, BrowseTarget target) {
    if (gui.dialog_open || busy(gui))
        return;
    gui.dialog_target = target;
    gui.dialog_open = true;
    gui.dialog_done = false;
    gui.dialog_path.clear();
    gui.dialog_error.clear();

    switch (target) {
        case BrowseTarget::Disc: gui.dialog_default = path_of(gui.disc).string(); break;
        case BrowseTarget::Data: gui.dialog_default = path_of(gui.data).string(); break;
        case BrowseTarget::Prepared: gui.dialog_default = path_of(gui.prepared).string(); break;
        case BrowseTarget::Saves: gui.dialog_default = path_of(gui.saves).string(); break;
        case BrowseTarget::Bios: gui.dialog_default = path_of(gui.bios).string(); break;
        case BrowseTarget::Boot: gui.dialog_default = path_of(gui.boot).string(); break;
        case BrowseTarget::Wav: gui.dialog_default = path_of(gui.wav).string(); break;
        case BrowseTarget::Screenshot: gui.dialog_default = path_of(gui.screenshot).string(); break;
        case BrowseTarget::Record: gui.dialog_default = path_of(gui.record).string(); break;
        case BrowseTarget::Replay: gui.dialog_default = path_of(gui.replay).string(); break;
        case BrowseTarget::None: break;
    }

    const char* location = gui.dialog_default.empty() ? nullptr : gui.dialog_default.c_str();
    if (target == BrowseTarget::Data || target == BrowseTarget::Prepared || target == BrowseTarget::Saves) {
        SDL_ShowOpenFolderDialog(dialog_callback, &gui, gui.window, location, false);
    }
    else {
        static const SDL_DialogFileFilter disc_filters[] = {{"Ikaruga disc images", "gdi;chd"},
                                                             {"All files", "*"}};
        static const SDL_DialogFileFilter image_filters[] = {{"Disc images", "gdi;chd"},
                                                              {"All files", "*"}};
        static const SDL_DialogFileFilter binary_filters[] = {{"Binary files", "bin"}, {"All files", "*"}};
        static const SDL_DialogFileFilter all_filters[] = {{"All files", "*"}};
        const SDL_DialogFileFilter* filters = all_filters;
        int filter_count = 1;
        if (target == BrowseTarget::Disc) {
            filters = disc_filters;
            filter_count = 2;
        }
        else if (target == BrowseTarget::Bios || target == BrowseTarget::Boot ||
                 target == BrowseTarget::Screenshot) {
            filters = target == BrowseTarget::Screenshot ? image_filters : binary_filters;
            filter_count = 2;
        }
        SDL_ShowOpenFileDialog(dialog_callback, &gui, gui.window, filters, filter_count, location, false);
    }
}

void poll_dialog(Gui& gui) {
    std::string path, error;
    BrowseTarget target = BrowseTarget::None;
    {
        std::lock_guard<std::mutex> lock(gui.dialog_mutex);
        if (!gui.dialog_done)
            return;
        gui.dialog_done = false;
        gui.dialog_open = false;
        path = std::move(gui.dialog_path);
        error = std::move(gui.dialog_error);
        target = gui.dialog_target;
        gui.dialog_path.clear();
        gui.dialog_error.clear();
        gui.dialog_target = BrowseTarget::None;
    }
    if (!error.empty()) {
        set_message(gui, "File dialog: " + error);
        return;
    }
    if (path.empty())
        return;
    set_path_field(gui, target, path);
    if (target == BrowseTarget::Data) {
        discover_discs(gui);
        if (gui.disc[0] == '\0' && gui.detected.size() == 1)
            copy_path(gui.disc, gui.detected.front());
    }
    set_message(gui, "Ready.");
}

std::vector<std::string> split_arguments(const std::string& text) {
    std::vector<std::string> out;
    std::string word;
    char quote = 0;
    for (const char c : text) {
        if (quote) {
            if (c == quote)
                quote = 0;
            else
                word += c;
        }
        else if (c == '\'' || c == '"') {
            quote = c;
        }
        else if (std::isspace(static_cast<unsigned char>(c))) {
            if (!word.empty()) {
                out.push_back(word);
                word.clear();
            }
        }
        else {
            word += c;
        }
    }
    if (!word.empty())
        out.push_back(std::move(word));
    return out;
}

struct Job {
    fs::path disc;
    fs::path data;
    fs::path prepared;
    bool extract = true;
    bool rebuild = false;
    bool fullscreen = false;
    bool unthrottled = false;
    bool audio = true;
    bool gamepads = true;
    int tate = 0;
    int scale = 0;
    int frames = 0;
    std::string bios;
    std::string boot;
    std::string saves;
    std::string wav;
    std::string screenshot;
    std::string screenshot_at;
    std::string press;
    std::string record;
    std::string replay;
    std::string extra;
};

void append(std::vector<std::string>& args, const char* name, const std::string& value) {
    args.emplace_back(name);
    args.emplace_back(value);
}

void run_job(Gui& gui, Job job) {
    try {
        launcher::PrepareRequest request;
        request.disc = job.disc;
        request.data_dir = job.data;
        request.prepared_dir = job.prepared;
        request.extract = job.extract;
        request.rebuild = job.rebuild;

        launcher::PreparedDisc prepared;
        std::string error;
        if (!launcher::prepare(request, prepared, error)) {
            set_state(gui, State::Failed, "The disc could not be prepared.", error);
            return;
        }

        const fs::path game = launcher::find_game(base_dir());
        if (game.empty()) {
            set_state(gui, State::Failed, "The game executable was not found.",
                      "ikaruga is not next to the launcher. Build the game first.");
            return;
        }

        std::vector<std::string> args = {game.string(), "--disc", prepared.gdi.string()};
        // The game always gets a window: without --window it runs scripted, with nothing to see.
        args.emplace_back("--window");
        if (job.fullscreen)
            args.emplace_back("--fullscreen");
        if (job.tate > 0) {
            static const char* const kTate[] = {"auto", "off", "left", "right"};
            append(args, "--tate", kTate[std::min(job.tate, 4) - 1]);
        }
        if (job.unthrottled)
            args.emplace_back("--unthrottled");
        if (job.scale > 0)
            append(args, "--scale", std::to_string(job.scale));
        if (!job.audio)
            args.emplace_back("--no-audio");
        if (!job.gamepads)
            args.emplace_back("--no-gamepads");
        if (job.frames > 0)
            append(args, "--frames", std::to_string(job.frames));
        if (!job.bios.empty())
            append(args, "--bios", job.bios);
        if (!job.boot.empty())
            append(args, "--boot", job.boot);
        if (!job.saves.empty())
            append(args, "--saves", job.saves);
        if (!job.wav.empty())
            append(args, "--wav", job.wav);
        if (!job.screenshot_at.empty())
            append(args, "--screenshot-at", job.screenshot_at);
        if (!job.screenshot.empty())
            append(args, "--screenshot-path", job.screenshot);
        if (!job.press.empty())
            append(args, "--press", job.press);
        if (!job.record.empty())
            append(args, "--record-input", job.record);
        if (!job.replay.empty())
            append(args, "--replay-input", job.replay);
        for (const std::string& extra : split_arguments(job.extra))
            args.push_back(extra);

        std::vector<const char*> raw;
        raw.reserve(args.size() + 1);
        for (const std::string& arg : args)
            raw.push_back(arg.c_str());
        raw.push_back(nullptr);

        std::error_code ec;
        fs::create_directories(job.data, ec);
        if (!job.saves.empty())
            fs::create_directories(job.saves, ec);

        SDL_PropertiesID properties = SDL_CreateProperties();
        SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, raw.data());
        SDL_SetStringProperty(properties, SDL_PROP_PROCESS_CREATE_WORKING_DIRECTORY_STRING,
                              job.data.string().c_str());
        SDL_Process* process = SDL_CreateProcessWithProperties(properties);
        SDL_DestroyProperties(properties);
        if (!process) {
            set_state(gui, State::Failed, "Ikaruga could not be started.", SDL_GetError());
            return;
        }

        set_state(gui, State::Running, "Ikaruga is running. Close its window to return here.");
        int exit_code = 0;
        const bool waited = SDL_WaitProcess(process, true, &exit_code);
        SDL_DestroyProcess(process);
        if (!waited) {
            set_state(gui, State::Failed, "Ikaruga could not be waited for.", SDL_GetError());
            return;
        }
        if (exit_code == 0)
            set_state(gui, State::Done, "Ikaruga closed. Ready to launch again.");
        else
            set_state(gui, State::Failed, "Ikaruga exited with an error.", "Exit code " + std::to_string(exit_code));
    }
    catch (const std::exception& e) {
        set_state(gui, State::Failed, "The launcher hit an error.", e.what());
    }
    catch (...) {
        set_state(gui, State::Failed, "The launcher hit an unknown error.", {});
    }
}

void start_launch(Gui& gui) {
    if (busy(gui) || gui.dialog_open)
        return;
    if (gui.worker.joinable())
        gui.worker.join();

    const fs::path disc = path_of(gui.disc);
    const fs::path data = path_of(gui.data);
    const std::string ext = lower(disc.extension().string());
    std::error_code ec;
    if (disc.empty() || !fs::is_regular_file(disc, ec)) {
        set_state(gui, State::Failed, "Choose a disc image first.", "The disc file does not exist.");
        return;
    }
    if (ext != ".gdi" && ext != ".chd") {
        set_state(gui, State::Failed, "Choose a disc image first.", "The disc must be a .gdi or .chd file.");
        return;
    }
    if (data.empty()) {
        set_state(gui, State::Failed, "Choose a data folder first.", "The data folder cannot be empty.");
        return;
    }

    Job job;
    job.disc = disc;
    job.data = data;
    job.prepared = path_of(gui.prepared);
    job.extract = gui.extract;
    job.rebuild = gui.rebuild;
    job.fullscreen = gui.fullscreen;
    job.tate = gui.tate;
    job.unthrottled = gui.unthrottled;
    job.audio = gui.audio;
    job.gamepads = gui.gamepads;
    job.scale = gui.scale;
    job.frames = std::max(0, gui.frames);
    job.bios = gui.bios.data();
    job.boot = gui.boot.data();
    job.saves = gui.saves.data();
    job.wav = gui.wav.data();
    job.screenshot = gui.screenshot.data();
    job.screenshot_at = gui.screenshot_at.data();
    job.press = gui.press.data();
    job.record = gui.record.data();
    job.replay = gui.replay.data();
    job.extra = gui.extra.data();

    save_settings(gui);
    set_state(gui, State::Preparing, "Preparing the disc image and game files...");
    gui.minimized = false;
    gui.worker = std::thread(run_job, std::ref(gui), std::move(job));
}

bool can_launch(const Gui& gui) {
    return gui.disc[0] != '\0' && gui.data[0] != '\0' && !busy(gui) && !gui.dialog_open;
}

void path_row(Gui& gui, const char* id, const char* label, PathText& field, BrowseTarget target) {
    ImGui::PushID(id);
    ImGui::TextDisabled("%s", label);
    const float button_width = 100.0f;
    ImGui::SetNextItemWidth(std::max(80.0f, ImGui::GetContentRegionAvail().x - button_width - 8.0f));
    ImGui::InputText("##path", field.data(), field.size());
    ImGui::SameLine();
    ImGui::BeginDisabled(gui.dialog_open || busy(gui));
    if (ImGui::Button("Browse...", ImVec2(button_width, 0)))
        begin_browse(gui, target);
    ImGui::EndDisabled();
    ImGui::PopID();
}

void draw_ui(Gui& gui) {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(26, 22));
    ImGui::Begin("##ikaruga-launcher", nullptr, flags);
    ImGui::PopStyleVar();

    ImGui::TextColored(kAccent, "IKARUGA");
    ImGui::SameLine();
    ImGui::TextDisabled("native port launcher");
    ImGui::TextWrapped("Load your own GDI or CHD dump, set the launch options, and start the game.");
    ImGui::Spacing();

    ImGui::BeginChild("body", ImVec2(0, -126), false);
    ImGui::SeparatorText("DISC IMAGE");
    path_row(gui, "disc", "GDI or CHD image", gui.disc, BrowseTarget::Disc);
    ImGui::TextDisabled("You can also drop a .gdi or .chd file onto this window.");

    if (!gui.detected.empty()) {
        ImGui::SetNextItemWidth(std::max(200.0f, ImGui::GetContentRegionAvail().x - 120.0f));
        if (ImGui::BeginCombo("##detected", "Detected discs...", ImGuiComboFlags_HeightSmall)) {
            for (const fs::path& path : gui.detected) {
                const std::string name = path.string();
                if (ImGui::Selectable(name.c_str()))
                    copy_path(gui.disc, path);
                if (lower(path.extension().string()) == lower(path_of(gui.disc).extension().string()) &&
                    path == path_of(gui.disc))
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }

    const std::string kind = disc_kind(gui.disc);
    if (!kind.empty()) {
        const fs::path disc = path_of(gui.disc);
        if (fs::is_regular_file(disc)) {
            ImGui::TextColored(kMuted, "%s · %s", kind.c_str(), disc.filename().string().c_str());
        }
        else {
            ImGui::TextColored(kError, "The selected disc file does not exist.");
        }
    }
    else if (gui.disc[0] != '\0') {
        ImGui::TextColored(kError, "The selected file must be a .gdi or .chd image.");
    }

    ImGui::Spacing();
    ImGui::SeparatorText("LAUNCH OPTIONS");
    const char* scales[] = {"Auto (game graphics setting)", "1X · 640x480", "2X · 1280x960",
                            "3X · 1920x1440", "4X · 2560x1920", "5X · 3200x2400",
                            "6X · 3840x2880"};
    ImGui::SetNextItemWidth(260);
    ImGui::Combo("Resolution", &gui.scale, scales, 7);
    ImGui::SameLine();
    ImGui::Checkbox("Fullscreen", &gui.fullscreen);
    ImGui::SameLine();
    ImGui::Checkbox("Unthrottled", &gui.unthrottled);
    ImGui::Checkbox("Audio", &gui.audio);
    ImGui::SameLine();
    ImGui::Checkbox("Gamepads", &gui.gamepads);
    ImGui::SameLine();
    ImGui::Checkbox("Extract disc files", &gui.extract);
    ImGui::SameLine();
    ImGui::BeginDisabled(kind != "CHD");
    ImGui::Checkbox("Rebuild CHD cache", &gui.rebuild);
    ImGui::EndDisabled();
    const char* tates[] = {"Tate: the game's setting", "Tate: auto (a portrait window)", "Tate: off",
                           "Tate: rotate left", "Tate: rotate right"};
    ImGui::SetNextItemWidth(260);
    ImGui::Combo("Rotated monitor", &gui.tate, tates, 5);
    ImGui::TextDisabled("Fullscreen starts the game full screen; unthrottled runs it as fast as the machine can.");
    ImGui::TextDisabled("Tate turns the picture for a monitor stood on its side, while the game's VERTICAL SCREEN option is on.");

    ImGui::Spacing();
    ImGui::SeparatorText("DATA");
    path_row(gui, "data", "Data folder (disc extraction and CHD cache)", gui.data, BrowseTarget::Data);
    path_row(gui, "prepared", "Prepared disc folder (empty: automatic)", gui.prepared, BrowseTarget::Prepared);

    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Advanced game options")) {
        path_row(gui, "saves", "Save folder (empty: the game's settings folder)", gui.saves, BrowseTarget::Saves);
        path_row(gui, "bios", "Boot ROM for system text (optional)", gui.bios, BrowseTarget::Bios);
        path_row(gui, "boot", "Custom 1ST_READ.BIN (optional)", gui.boot, BrowseTarget::Boot);

        ImGui::TextDisabled("Frame limit (0 = until the game exits)");
        ImGui::SetNextItemWidth(180);
        ImGui::InputInt("##frames", &gui.frames, 1, 60);
        if (gui.frames < 0)
            gui.frames = 0;

        ImGui::TextDisabled("Screenshot frames (for example: 300,800)");
        ImGui::SetNextItemWidth(300);
        ImGui::InputText("##screenshot-at", gui.screenshot_at.data(), gui.screenshot_at.size());
        path_row(gui, "screenshot", "Screenshot output path", gui.screenshot, BrowseTarget::Screenshot);
        path_row(gui, "wav", "Record sound to WAV (optional)", gui.wav, BrowseTarget::Wav);

        ImGui::TextDisabled("Scripted presses (for example: start@600,a@700)");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##press", gui.press.data(), gui.press.size());
        path_row(gui, "record", "Record input to file", gui.record, BrowseTarget::Record);
        path_row(gui, "replay", "Replay input from file", gui.replay, BrowseTarget::Replay);

        ImGui::TextDisabled("Extra game arguments (quoting is supported)");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextMultiline("##extra", gui.extra.data(), gui.extra.size(), ImVec2(-FLT_MIN, 60));
    }

    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Disc preparation")) {
        ImGui::TextDisabled("The data folder holds game/extracted and game/prepared. For a GDI, the "
                            "selected .gdi and its track files are used in place.");
        ImGui::TextDisabled("A CHD is unpacked into the prepared folder. Extraction is skipped only when "
                            "the files are already there.");
    }
    ImGui::EndChild();

    ImGui::Separator();
    const State state = gui.state.load();
    auto [status, error] = state_text(gui);
    const ImVec4 color = state == State::Failed ? kError
                         : (state == State::Running || state == State::Preparing) ? kAccent
                                                                                 : kReady;
    ImGui::TextColored(color, "%s", status.c_str());
    if (!error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kError);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextWrapped("%s", error.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();
    ImGui::BeginDisabled(!can_launch(gui));
    if (ImGui::Button(state == State::Preparing || state == State::Running ? "Working..." : "Launch Ikaruga",
                      ImVec2(220, 40)))
        start_launch(gui);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Quit", ImVec2(100, 40)))
        gui.quit_requested = true;
    ImGui::SameLine();
    ImGui::TextDisabled("Settings are saved when you launch or quit.");
    ImGui::End();
}

void follow_process_window(Gui& gui) {
    const State state = gui.state.load();
    if (state == State::Running && !gui.minimized) {
        SDL_MinimizeWindow(gui.window);
        gui.minimized = true;
    }
    else if ((state == State::Done || state == State::Failed) && gui.minimized) {
        SDL_RestoreWindow(gui.window);
        SDL_RaiseWindow(gui.window);
        gui.minimized = false;
    }
}

void style() {
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.ChildRounding = 8.0f;
    style.FrameRounding = 5.0f;
    style.PopupRounding = 6.0f;
    style.GrabRounding = 5.0f;
    style.WindowBorderSize = 0.0f;
    style.FrameBorderSize = 0.0f;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.055f, 0.060f, 0.075f, 1.0f);
    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.070f, 0.075f, 0.092f, 1.0f);
    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.12f, 0.13f, 0.16f, 1.0f);
    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.18f, 0.19f, 0.23f, 1.0f);
    style.Colors[ImGuiCol_Button] = ImVec4(0.20f, 0.15f, 0.16f, 1.0f);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.36f, 0.20f, 0.20f, 1.0f);
    style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.48f, 0.23f, 0.22f, 1.0f);
    style.Colors[ImGuiCol_Header] = ImVec4(0.22f, 0.15f, 0.16f, 1.0f);
    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.36f, 0.20f, 0.20f, 1.0f);
    style.Colors[ImGuiCol_Separator] = ImVec4(0.20f, 0.21f, 0.25f, 1.0f);
    style.FontScaleMain = 1.12f;
    style.ScaleAllSizes(1.08f);
    style.WindowPadding = ImVec2(24, 20);
}

}  // namespace

namespace launcher {

int run_gui(const fs::path& initial_disc) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "launcher: cannot start SDL: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* window = SDL_CreateWindow("Ikaruga Launcher", 980, 760,
                                          SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window) {
        std::fprintf(stderr, "launcher: cannot create the window: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_SetWindowMinimumSize(window, 760, 560);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer) {
        std::fprintf(stderr, "launcher: cannot create the renderer: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_SetRenderVSync(renderer, 1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;
    style();
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    Gui gui;
    gui.window = window;
    gui.renderer = renderer;
    const fs::path pref = pref_dir();
    if (!pref.empty())
        gui.settings_file = pref / "launcher.ini";
    copy_path(gui.data, default_data_dir());
    load_settings(gui);
    if (gui.data[0] == '\0')
        copy_path(gui.data, default_data_dir());
    if (!initial_disc.empty())
        copy_path(gui.disc, initial_disc);
    discover_discs(gui);
    if (gui.disc[0] == '\0' && gui.detected.size() == 1)
        copy_path(gui.disc, gui.detected.front());
    if (gui.bios[0] == '\0') {
        const fs::path default_bios = path_of(gui.data) / "bios" / "dc_bios.bin";
        if (fs::is_regular_file(default_bios))
            copy_path(gui.bios, default_bios);
    }

    bool quit = false;
    while (!quit) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT) {
                gui.quit_requested = true;
            }
            else if (event.type == SDL_EVENT_DROP_FILE && event.drop.data) {
                const fs::path dropped(event.drop.data);
                const std::string kind = disc_kind(event.drop.data);
                if (kind.empty()) {
                    set_state(gui, gui.state.load(), "Drop a .gdi or .chd file to choose a disc.",
                              "That file is not a supported disc image.");
                }
                else {
                    copy_path(gui.disc, dropped);
                    set_state(gui, State::Idle, "Disc selected. Ready to launch.");
                }
            }
        }

        poll_dialog(gui);
        if (gui.state == State::Done || gui.state == State::Failed) {
            if (gui.worker.joinable())
                gui.worker.join();
        }
        if (gui.quit_requested) {
            gui.quit_requested = false;
            if (busy(gui))
                set_message(gui, "Ikaruga is still running. Close the game before closing the launcher.");
            else
                quit = true;
        }
        if (quit)
            break;

        follow_process_window(gui);

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        draw_ui(gui);
        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 14, 15, 19, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }

    if (gui.worker.joinable())
        gui.worker.join();
    save_settings(gui);
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}

}  // namespace launcher
