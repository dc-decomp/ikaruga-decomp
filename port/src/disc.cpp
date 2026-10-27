#include "disc.h"

#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>

#include "hw.h"

namespace disc {
namespace {

struct File {
    uint32_t lba = 0;
    uint32_t sector_size = 2352;
    bool data = false;
    std::ifstream file;
};
std::vector<File> files;

uint32_t le32(const uint8_t* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

Entry entry(const uint8_t* record) {
    Entry e;
    e.lba = le32(record + 2);
    e.size = le32(record + 10);
    e.directory = (record[25] & 2) != 0;
    const int length = record[32];
    for (int i = 0; i < length && record[33 + i] != ';'; ++i)
        e.name += static_cast<char>(std::toupper(record[33 + i]));
    return e;
}

// The high-density area's data track, which holds IP.BIN and the file system, or 0.
uint32_t high_density_start() {
    uint32_t start = 0;
    for (const File& f : files)
        if (f.data && f.lba >= 45000)
            start = f.lba;
    return start;
}

}  // namespace

bool open(const std::string& gdi_path, std::string& error) {
    std::ifstream gdi(gdi_path);
    if (!gdi) {
        error = "cannot read " + gdi_path;
        return false;
    }
    const std::string dir = gdi_path.find('/') == std::string::npos ? "." : gdi_path.substr(0, gdi_path.rfind('/'));
    files.clear();
    int n = 0;
    gdi >> n;
    std::string line;
    std::getline(gdi, line);
    for (int i = 0; i < n && std::getline(gdi, line);) {
        // A track: its number, first sector, type, sector size, file name (quoted when it has
        // spaces) and offset.
        std::istringstream in(line);
        int number = 0, type = 0, size = 0;
        uint32_t lba = 0;
        std::string name;
        if (!(in >> number >> lba >> type >> size >> std::ws))
            continue;
        if (in.peek() == '"') {
            in.get();
            std::getline(in, name, '"');
        }
        else
            in >> name;
        ++i;
        File f;
        f.lba = lba;
        f.sector_size = static_cast<uint32_t>(size);
        f.data = type == 4;
        f.file.open(dir + "/" + name, std::ios::binary);
        if (!f.file) {
            error = "cannot read " + dir + "/" + name;
            return false;
        }
        files.push_back(std::move(f));
    }
    return true;
}

std::vector<Track> tracks() {
    std::vector<Track> out;
    for (const File& f : files)
        out.push_back({f.lba, f.data});
    return out;
}

bool read_sector(uint32_t lba, uint8_t* out) {
    File* t = nullptr;
    for (File& f : files)
        if (f.data && f.lba <= lba)
            t = &f;
    if (!t)
        return false;
    uint8_t raw[2352];
    t->file.clear();
    t->file.seekg(static_cast<std::streamoff>(lba - t->lba) * t->sector_size);
    if (!t->file.read(reinterpret_cast<char*>(raw), t->sector_size))
        return false;
    ++hw::stats().gd_sectors;
    if (t->sector_size == 2048) {
        std::memcpy(out, raw, 2048);
        return true;
    }
    const int mode = raw[15];
    std::memcpy(out, raw + (mode == 2 ? 24 : 16), 2048);
    return true;
}

bool root(Entry& out) {
    // The volume descriptors start 16 sectors into the high-density data track.
    const uint32_t start = high_density_start();
    uint8_t pvd[2048];
    if (!start || !read_sector(start + 16, pvd) || pvd[0] != 1 || std::memcmp(pvd + 1, "CD001", 5) != 0)
        return false;
    out = entry(pvd + 156);
    out.name.clear();
    return true;
}

std::vector<Entry> list(const Entry& directory) {
    std::vector<Entry> out;
    uint8_t sector[2048];
    for (uint32_t done = 0; done < directory.size; done += 2048) {
        if (!read_sector(directory.lba + done / 2048, sector))
            break;
        // A record never crosses a sector; a zero length ends the sector's records.
        for (uint32_t at = 0; at < 2048 && sector[at] != 0; at += sector[at]) {
            const uint8_t* record = sector + at;
            if (record[32] == 1 && (record[33] == 0 || record[33] == 1))
                continue;  // "." and ".."
            out.push_back(entry(record));
        }
    }
    return out;
}

// The port's own reads, which the game's count of sectors leaves out.
bool read_bootstrap(std::vector<uint8_t>& out) {
    const uint64_t counted = hw::stats().gd_sectors;
    const uint32_t start = high_density_start();
    out.assign(16 * 2048, 0);
    bool ok = start != 0;
    for (uint32_t i = 0; i < 16 && ok; ++i)
        ok = read_sector(start + i, out.data() + 2048 * i);
    hw::stats().gd_sectors = counted;
    return ok;
}

bool read_file(const std::string& name, std::vector<uint8_t>& out) {
    const uint64_t counted = hw::stats().gd_sectors;
    Entry r;
    bool ok = false;
    if (root(r))
        for (const Entry& e : list(r)) {
            if (e.directory || e.name != name)
                continue;
            out.assign((e.size + 2047) / 2048 * 2048, 0);
            ok = true;
            for (uint32_t i = 0; i < out.size() / 2048 && ok; ++i)
                ok = read_sector(e.lba + i, out.data() + 2048 * i);
            out.resize(e.size);
            break;
        }
    hw::stats().gd_sectors = counted;
    return ok;
}

}  // namespace disc
