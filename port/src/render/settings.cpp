#include "render/settings.h"

#include <SDL3/SDL.h>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <utility>
#include <vector>

namespace render {
namespace {

struct Entry {
    const char* key;
    bool Tweaks::*value;
};
constexpr Entry kEntries[] = {
    {"infinite_lives", &Tweaks::infinite_lives},
    {"infinite_charge", &Tweaks::infinite_charge},
    {"half_bullets", &Tweaks::half_bullets},
    {"no_press_start", &Tweaks::no_press_start},
};

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// Each `key = value` line's key and value.
std::vector<std::pair<std::string, std::string>> lines_of(const std::string& text) {
    std::vector<std::pair<std::string, std::string>> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        const size_t eq = line.find('=');
        if (eq != std::string::npos)
            out.emplace_back(trim(line.substr(0, eq)), trim(line.substr(eq + 1)));
    }
    return out;
}

// A number from `low` to `high`, or `fallback` for anything else.
unsigned number(const std::string& value, unsigned low, unsigned high, unsigned fallback) {
    char* end = nullptr;
    const unsigned long n = std::strtoul(value.c_str(), &end, 10);
    return end != value.c_str() && *end == 0 && n >= low && n <= high ? static_cast<unsigned>(n) : fallback;
}

}  // namespace

Tweaks Tweaks::parse(const std::string& text) {
    Tweaks t;
    for (const auto& [key, value] : lines_of(text))
        for (const Entry& e : kEntries)
            if (key == e.key)
                t.*e.value = value == "1";
    return t;
}

std::string Tweaks::text() const {
    std::ostringstream o;
    for (const Entry& e : kEntries) o << e.key << " = " << (this->*e.value ? 1 : 0) << "\n";
    return o.str();
}

Graphics Graphics::parse(const std::string& text) {
    Graphics g;
    for (const auto& [key, value] : lines_of(text)) {
        if (key == "scale")
            g.scale = number(value, 1, kMaxScale, g.scale);
        else if (key == "bloom")
            g.bloom = number(value, 0, kMaxBloom, g.bloom);
        else if (key == "anisotropy") {
            const unsigned n = number(value, 1, kMaxAnisotropy, g.anisotropy);
            g.anisotropy = (n & (n - 1)) == 0 ? n : g.anisotropy;
        }
        else if (key == "fullscreen")
            g.fullscreen = value == "1";
        else if (key == "vsync")
            g.vsync = value == "1";
        else if (key == "hud")
            g.hud = value == "1";
    }
    return g;
}

std::string Graphics::text() const {
    return "scale = " + std::to_string(scale) + "\nbloom = " + std::to_string(bloom) + "\nanisotropy = " +
           std::to_string(anisotropy) + "\nfullscreen = " + (fullscreen ? "1" : "0") + "\nvsync = " + (vsync ? "1" : "0") +
           "\nhud = " + (hud ? "1" : "0") + "\n";
}

std::string settings_path(const char* name) {
    char* dir = SDL_GetPrefPath("ikaruga", "ikaruga");
    if (!dir)
        return {};
    std::string path = std::string(dir) + name;
    SDL_free(dir);
    return path;
}

std::string read_settings(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return in ? std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()) : std::string();
}

bool write_settings(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    out << text;
    return static_cast<bool>(out);
}

}  // namespace render
