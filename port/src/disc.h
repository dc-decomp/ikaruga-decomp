// The disc image: a GDI's tracks, as `dcdisc chd2gdi` writes them, and the ISO 9660 file system on
// the high-density data track. The native file system (files.cpp) reads the game's files through
// it, and the boot ROM's drive commands (bios.cpp) read its sectors.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace disc {

bool open(const std::string& gdi_path, std::string& error);

struct Track {
    uint32_t lba = 0;
    bool data = false;
};
std::vector<Track> tracks();

// The 2048 bytes of user data of the sector at `lba`.
bool read_sector(uint32_t lba, uint8_t* out);

// A file or directory of the file system. `lba` is the first sector; ISO 9660 records it as an
// address on the whole disc, so the high-density area's files start after LBA 45000.
struct Entry {
    std::string name;  // upper case, without the ";1" version
    uint32_t lba = 0;
    uint32_t size = 0;
    bool directory = false;
};
// The root directory, from the primary volume descriptor.
bool root(Entry& out);
// The entries of a directory, without "." and "..".
std::vector<Entry> list(const Entry& directory);

}  // namespace disc
