#pragma once

#include <filesystem>
#include <string>

namespace launcher {

struct PrepareRequest {
    std::filesystem::path disc;
    std::filesystem::path data_dir;
    std::filesystem::path prepared_dir;  // Empty: data_dir/game/prepared/<disc name>.
    bool extract = true;
    bool rebuild = false;
};

struct PreparedDisc {
    std::filesystem::path gdi;
    std::string description;
};

// Unpacks a CHD when needed and extracts the disc's files into data_dir/game/extracted.
bool prepare(const PrepareRequest& request, PreparedDisc& prepared, std::string& error);

// The game next to the launcher, or in the build directories used by the port.
std::filesystem::path find_game(const std::filesystem::path& where);

// The graphical launcher. With a disc path, opens with that disc selected.
int run_gui(const std::filesystem::path& initial_disc = {});

}  // namespace launcher
