// The memory card, natively: Shinobi's backup library at the functions the game calls (bu*). The
// card is a directory on the host. Each file in it holds the image that buMakeBackupFileImage
// built, in the VMS layout that Dreamcast save tools read, under the game's file name with ".VMS"
// added. A request is complete when it returns, so the card is never busy. The library's own file
// system and the Maple memory card beneath it are no longer reached for the game's files.
#include "saves.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "decomp.h"
#include "diff.h"

namespace fs = std::filesystem;

namespace saves {
namespace {
std::string directory = "saves";
}

void set_directory(const std::string& path) { directory = path; }

}  // namespace saves

namespace game {
namespace {

// The SDK's result codes.
constexpr Sint32 kOk = 0;
constexpr Sint32 kNoDisk = static_cast<Sint32>(0xFFFFFF01u);
constexpr Sint32 kDiskFull = static_cast<Sint32>(0xFFFFFF04u);
constexpr Sint32 kFileNotFound = static_cast<Sint32>(0xFFFFFF05u);
constexpr Sint32 kCannotCreate = static_cast<Sint32>(0xFFFFFF08u);
constexpr Sint32 kWriteError = static_cast<Sint32>(0xFFFFFF40u);

// A 128 KB card has 200 blocks of 512 bytes for files. The game offers to make its file only
// when 34 blocks are free.
constexpr std::uintmax_t kBlock = 512, kBlocks = 200;

Sint32 last_error = kOk;

// IKARUGA_LOG_FILES: each call with its result.
bool logging() {
    static const bool on = std::getenv("IKARUGA_LOG_FILES") != nullptr;
    return on;
}

// The game's file name, at most 12 characters, as a host file name.
std::string file_name(gptr<char> name) {
    std::string s;
    for (uint32_t a = name.a; s.size() < 12; ++a) {
        const char c = static_cast<char>(guest::load<uint8_t>(a));
        if (c == 0)
            break;
        const bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        s += plain ? c : '_';
    }
    return s + ".VMS";
}

fs::path path_of(gptr<char> name) { return fs::path(saves::directory) / file_name(name); }

// Blocks that the files on the card take, but for the one at `except`.
std::uintmax_t used_blocks(const fs::path& except) {
    std::uintmax_t used = 0;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(saves::directory, ec))
        if (e.is_regular_file(ec) && e.path().extension() == ".VMS" && e.path() != except)
            used += (e.file_size(ec) + kBlock - 1) / kBlock;
    return used;
}

void log(const char* what, gptr<char> name, Sint32 result) {
    if (logging())
        std::fprintf(stderr, "saves: %s %s: %d\n", what, file_name(name).c_str(), result);
}

}  // namespace

Sint32 buInit(Sint32, Sint32, gptr<void>, BU_INITCALLBACK func) {
    if (func.a)
        guest::call(func.a, CallArgs{});
    return kOk;
}

// Drive 0 is the card in the first controller's first slot, the only one there is.
Sint32 buIsReady(Sint32 drive) { return drive == 0 ? 1 : 0; }
Sint32 buStat(Sint32) { return 0; }
Sint32 buGetLastError(Sint32) { return last_error; }

Sint32 buGetFileSize(Sint32 drive, gptr<char> fname) {
    std::error_code ec;
    const std::uintmax_t size = fs::file_size(path_of(fname), ec);
    const Sint32 r = drive != 0 ? kNoDisk : ec ? kFileNotFound : static_cast<Sint32>((size + kBlock - 1) / kBlock);
    log("size of", fname, r);
    return r;
}

Sint32 buLoadFile(Sint32 drive, gptr<char> fname, gptr<void> buf, Uint32 nblock) {
    diff::guard("reads the memory card");
    std::ifstream in(path_of(fname), std::ios::binary);
    if (drive != 0 || !in) {
        last_error = drive != 0 ? kNoDisk : kFileNotFound;
        log("load", fname, last_error);
        return last_error;
    }
    std::vector<char> data(nblock * kBlock);
    in.read(data.data(), static_cast<std::streamsize>(data.size()));
    const auto n = static_cast<unsigned>(in.gcount());
    if (uint8_t* p = guest::direct(buf.a))
        std::copy(data.begin(), data.begin() + n, p);
    else
        guest::io_write_block(buf.a, data.data(), n);
    last_error = kOk;
    log("load", fname, last_error);
    return kOk;
}

Sint32 buSaveFile(Sint32 drive, gptr<char> fname, gptr<void> buf, Uint32 nblock, gptr<BUS_TIME>, Uint32) {
    diff::guard("writes the memory card");
    const fs::path path = path_of(fname);
    std::error_code ec;
    fs::create_directories(saves::directory, ec);
    if (drive != 0)
        last_error = kNoDisk;
    else if (ec)
        last_error = kCannotCreate;
    else if (used_blocks(path) + nblock > kBlocks)
        last_error = kDiskFull;
    else {
        // The new file replaces the old one only when it is complete.
        std::vector<char> data(nblock * kBlock);
        if (const uint8_t* p = guest::direct(buf.a))
            std::copy(p, p + data.size(), data.begin());
        else
            guest::io_read_block(buf.a, data.data(), static_cast<unsigned>(data.size()));
        const fs::path part = path.string() + ".part";
        std::ofstream out(part, std::ios::binary | std::ios::trunc);
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        out.close();
        if (out)
            fs::rename(part, path, ec);
        last_error = out && !ec ? kOk : kWriteError;
    }
    log("save", fname, last_error);
    return last_error;
}

// Whether the file is on the card: 0, or an error.
undefined4 FUN_8c05837c(int drive, gptr<char> fname, undefined4, undefined4, float, float, float, float) {
    std::error_code ec;
    const Sint32 r = drive != 0 ? kNoDisk : fs::exists(path_of(fname), ec) ? kOk : kFileNotFound;
    log("look for", fname, r);
    return static_cast<undefined4>(r);
}

// The card's free blocks.
int FUN_8c057e40(int drive, int, undefined4, undefined4, float, float, float, float) {
    if (drive != 0)
        return kNoDisk;
    const std::uintmax_t used = used_blocks(fs::path());
    return used >= kBlocks ? 0 : static_cast<int>(kBlocks - used);
}

}  // namespace game
