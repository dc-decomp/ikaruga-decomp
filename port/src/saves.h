// The memory card, natively (saves.cpp): a directory on the host that holds the game's files.
#pragma once

#include <string>

namespace saves {

// Where the card's files go. The directory is made when the game first saves.
void set_directory(const std::string& path);

}  // namespace saves
