// The GD-ROM file system that the game calls (Shinobi's gdFs functions), natively: the files come
// from the disc image's ISO 9660 file system (disc.h), and a read is complete when it returns.
// Shinobi's own file system, its GD server and the drive commands beneath them are no longer
// reached. The game keeps its directory tables and handles in memory it passes in; they are
// filled in the SDK's layout, in case the game reads them.
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "decomp.h"
#include "diff.h"
#include "disc.h"

namespace game {
namespace {

// The SDK's status and error codes.
constexpr Sint32 kStatComplete = 1;
constexpr Sint32 kErrOk = 0, kErrNotFound = -8, kErrRead = -1;
constexpr uint32_t kFadOffset = 150;  // a sector's frame address is its LBA + 150

// A directory table in the game's memory: dir_num, max_ent, dir_fad, pad, then 44-byte entries.
struct Directory {
    disc::Entry self;
    std::vector<disc::Entry> entries;
};
std::map<uint32_t, Directory> directories;  // by the table's address
uint32_t current = 0;                       // the table gdFsSetDir chose

struct Handle {
    disc::Entry file;
    uint32_t offset = 0;  // in sectors
};
std::map<uint32_t, Handle> handles;  // by the address of the handle in the game's work area
uint32_t work = 0;
Sint32 max_open = 0;
constexpr uint32_t kHandleSize = sizeof(GDS_FS_HANDLE);

void write_table(uint32_t table, const Directory& d) {
    const Sint32 max = guest::load<Sint32>(table + 4);
    const Sint32 n = std::min<Sint32>(max, static_cast<Sint32>(d.entries.size()));
    guest::store<Sint32>(table, n);
    guest::store<Sint32>(table + 8, static_cast<Sint32>(d.self.lba + kFadOffset));
    for (Sint32 i = 0; i < n; ++i) {
        const disc::Entry& e = d.entries[static_cast<std::size_t>(i)];
        const uint32_t at = table + 16 + 44 * static_cast<uint32_t>(i);
        guest::store<Sint32>(at, static_cast<Sint32>(e.lba + kFadOffset));
        guest::store<Sint32>(at + 4, static_cast<Sint32>(e.size));
        guest::store<uint8_t>(at + 8, e.directory ? 2 : 0);
        guest::store<uint8_t>(at + 9, 0);
        for (uint32_t c = 0; c < 32; ++c)
            guest::store<uint8_t>(at + 10 + c, c < e.name.size() ? static_cast<uint8_t>(e.name[c]) : 0);
    }
}

std::string upper(const std::string& s) {
    std::string out;
    for (char c : s) out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

std::string guest_string(uint32_t a) {
    std::string s;
    for (char c; (c = static_cast<char>(guest::load<uint8_t>(a))) != 0; ++a) s += c;
    return s;
}

// IKARUGA_LOG_FILES: each call with its result.
bool logging() {
    static const bool on = std::getenv("IKARUGA_LOG_FILES") != nullptr;
    return on;
}

// Looks a path up from a directory: "NAME", "DIR\NAME", or from the root "\DIR\NAME" and "\".
// The standard library's sizes are spelled out: in namespace game, size_t is the guest's 32 bits.
bool find(const std::string& path, uint32_t table, disc::Entry& out) {
    disc::Entry at;
    std::string rest = upper(path);
    if (!rest.empty() && (rest[0] == '\\' || rest[0] == '/')) {
        if (!disc::root(at))
            return false;
        rest.erase(0, 1);
    }
    else {
        const auto d = directories.find(table);
        if (d == directories.end())
            return false;
        at = d->second.self;
    }
    std::vector<disc::Entry> entries = disc::list(at);
    while (!rest.empty()) {
        const std::string::size_type sep = rest.find_first_of("\\/");
        const std::string name = rest.substr(0, sep);
        const auto it = std::find_if(entries.begin(), entries.end(), [&](const disc::Entry& e) { return e.name == name; });
        if (it == entries.end())
            return false;
        at = *it;
        if (sep == std::string::npos)
            break;
        entries = disc::list(at);
        rest.erase(0, sep + 1);
    }
    out = at;
    return true;
}

Sint32 read(GDFS gdfs, Sint32 nsct, gptr<void> buf) {
    const auto h = handles.find(gdfs.a);
    if (h == handles.end() || nsct < 0)
        return kErrRead;
    Handle& f = h->second;
    const uint32_t sectors = (f.file.size + 2047) / 2048;
    uint8_t sector[2048];
    for (Sint32 i = 0; i < nsct && f.offset < sectors; ++i, ++f.offset) {
        if (!disc::read_sector(f.file.lba + f.offset, sector))
            return kErrRead;
        const uint32_t dest = buf.a + 2048u * static_cast<uint32_t>(i);
        if (uint8_t* p = guest::direct(dest))
            std::memcpy(p, sector, 2048);
        else
            guest::io_write_block(dest, sector, 2048);
    }
    guest::store<Sint32>(gdfs.a + offsetof(GDS_FS_HANDLE, ofs), static_cast<Sint32>(f.offset));
    return kErrOk;
}

}  // namespace

void gdFsInit2(Sint32 max, gptr<void> gdfs_work, Sint32 max_dirent, gptr<void> dirbuf) {
    diff::guard("changes the file system's state");
    work = gdfs_work.a;
    max_open = max;
    // The root directory becomes the current one, in the table the game passed for it.
    const GDFS_DIRREC root = gdFsCreateDirhn(dirbuf, max_dirent);
    Directory d;
    if (disc::root(d.self)) {
        d.entries = disc::list(d.self);
        directories[root.a] = d;
        write_table(root.a, d);
    }
    current = root.a;
    if (logging())
        std::fprintf(stderr, "files: root directory at LBA %u in %08x: %zu entries\n", d.self.lba, root.a,
                     d.entries.size());

}

void gdFsFinish() {}
Sint32 gdFsMount() { return kErrOk; }
Sint32 gdFsReMount() { return kErrOk; }
Bool gdFsIsTrayOpen() { return 0; }

GDFS_DIRREC gdFsCreateDirhn(gptr<void> dirbuf, Sint32 max_dirent) {
    guest::store<Sint32>(dirbuf.a, 0);
    guest::store<Sint32>(dirbuf.a + 4, max_dirent);
    guest::store<Sint32>(dirbuf.a + 8, 0);
    guest::store<Sint32>(dirbuf.a + 12, 0);
    return GDFS_DIRREC(dirbuf.a);
}

Sint32 gdFsLoadDir(gptr<char> dirname, GDFS_DIRREC gf_dirrec) {
    diff::guard("changes the file system's state");
    Directory d;
    const bool found = find(guest_string(dirname.a), current, d.self) && d.self.directory;
    if (logging())
        std::fprintf(stderr, "files: load dir %s into %08x: %s\n", guest_string(dirname.a).c_str(), gf_dirrec.a,
                     found ? "ok" : "not found");
    if (!found)
        return kErrNotFound;
    d.entries = disc::list(d.self);
    directories[gf_dirrec.a] = d;
    write_table(gf_dirrec.a, d);
    return kErrOk;
}

Sint32 gdFsSetDir(GDFS_DIRREC gf_dirrec) {
    diff::guard("changes the file system's state");
    if (!directories.count(gf_dirrec.a))
        return kErrNotFound;
    current = gf_dirrec.a;
    return kErrOk;
}

GDFS gdFsOpen(gptr<char> fname, GDFS_DIRREC gf_dirrec) {
    diff::guard("changes the file system's state");
    disc::Entry file;
    const bool found = find(guest_string(fname.a), gf_dirrec.a ? gf_dirrec.a : current, file) && !file.directory;
    if (logging())
        std::fprintf(stderr, "files: open %s in %08x: %s, %u bytes\n", guest_string(fname.a).c_str(),
                     gf_dirrec.a ? gf_dirrec.a : current, found ? "ok" : "not found", file.size);
    if (!found)
        return GDFS(0u);
    for (Sint32 i = 0; i < max_open; ++i) {
        const uint32_t at = work + kHandleSize * static_cast<uint32_t>(i);
        if (handles.count(at))
            continue;
        handles[at] = Handle{file, 0};
        for (uint32_t b = 0; b < kHandleSize; b += 4) guest::store<uint32_t>(at + b, 0);
        guest::store<uint32_t>(at + offsetof(GDS_FS_HANDLE, wk), work);
        guest::store<Sint32>(at + offsetof(GDS_FS_HANDLE, fid), i);
        guest::store<Sint32>(at + offsetof(GDS_FS_HANDLE, fad), static_cast<Sint32>(file.lba + kFadOffset));
        guest::store<Sint32>(at + offsetof(GDS_FS_HANDLE, fsize), static_cast<Sint32>(file.size));
        guest::store<Sint32>(at + offsetof(GDS_FS_HANDLE, fsctsize), static_cast<Sint32>((file.size + 2047) / 2048));
        guest::store<Sint16>(at + offsetof(GDS_FS_HANDLE, used), 1);
        guest::store<Sint16>(at + offsetof(GDS_FS_HANDLE, stat), kStatComplete);
        return GDFS(at);
    }
    return GDFS(0u);
}

void gdFsClose(GDFS gdfs) {
    diff::guard("changes the file system's state");
    if (handles.erase(gdfs.a))
        guest::store<Sint16>(gdfs.a + offsetof(GDS_FS_HANDLE, used), 0);
}

Bool gdFsGetFileSize(GDFS gdfs, gptr<Sint32> fsize) {
    const auto h = handles.find(gdfs.a);
    if (h == handles.end())
        return 0;
    *fsize = static_cast<Sint32>(h->second.file.size);
    return 1;
}

Bool gdFsGetFileSctSize(GDFS gdfs, gptr<Sint32> fsctsize) {
    const auto h = handles.find(gdfs.a);
    if (h == handles.end())
        return 0;
    *fsctsize = static_cast<Sint32>((h->second.file.size + 2047) / 2048);
    return 1;
}

Sint32 gdFsRead(GDFS gdfs, Sint32 nsct, gptr<void> buf) {
    diff::guard("reads the disc");
    const Sint32 r = read(gdfs, nsct, buf);
    if (logging())
        std::fprintf(stderr, "files: read %d sectors of %08x to %08x: %d\n", nsct, gdfs.a, buf.a, r);
    return r;
}
Sint32 gdFsReqRd32(GDFS gdfs, Sint32 nsct, gptr<void> buf) {
    diff::guard("reads the disc");
    const Sint32 r = read(gdfs, nsct, buf);
    if (logging())
        std::fprintf(stderr, "files: request %d sectors of %08x to %08x: %d\n", nsct, gdfs.a, buf.a, r);
    return r;
}
Sint32 gdFsGetStat(GDFS gdfs) { return handles.count(gdfs.a) ? kStatComplete : kErrNotFound; }
Sint32 gdFsGetErrStat(GDFS) { return kErrOk; }
Sint32 gdFsStopRd(GDFS) { return kErrOk; }

}  // namespace game
