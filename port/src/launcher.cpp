// The launcher: takes the user's GDI or CHD, makes a GDI the port can play (unpacking a CHD with
// libchdr), extracts the disc's file system into game/extracted, and can start the game. With no
// arguments it opens the graphical launcher; with arguments it keeps the command-line interface.
#include <libchdr/chd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "launcher.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace {

constexpr uint64_t kFrameBytes = 2448;  // 2352 sector bytes + 96 subchannel bytes
constexpr uint32_t kSectorBytes = 2352;
constexpr uint32_t kDataBytes = 2048;

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string extension(const fs::path& p) { return lower(p.extension().string()); }

uint32_t le32(const uint8_t* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

void usage() {
    std::cout <<
        "usage: ikaruga-launcher [options] [DISC.gdi|DISC.chd]\n"
        "  --disc FILE       the disc image (a .gdi with its tracks, or a .chd)\n"
        "  --out DIR         where a .chd is unpacked (default game/prepared/<name>)\n"
        "  --extract DIR     where the disc's files go (default game/extracted; --no-extract to skip)\n"
        "  --no-extract      do not extract the disc's files\n"
        "  --info            show the disc's tracks and stop\n"
        "  --play            start ikaruga with the prepared disc\n"
        "  --                  pass the rest to ikaruga when --play is used\n"
        "\n"
        "With no DISC, lists the .gdi and .chd files in ROM/ and game/ and asks which to use.\n";
}

struct Track {
    int number = 0;
    std::string type;
    uint64_t frames = 0;
    uint64_t pad = 0;
    uint64_t pregap = 0;
    std::string pgtype = "MODE1";
    uint64_t postgap = 0;
    uint64_t chd_frame = 0;
    uint64_t lba = 0;
    uint32_t sector_bytes = kSectorBytes;
    bool audio = false;

    uint64_t sectors() const { return frames >= pad ? frames - pad : frames; }
    bool stored_pregap() const { return !pgtype.empty() && (pgtype[0] == 'V' || pgtype[0] == 'v'); }
    uint64_t padded_frames() const { return (frames + 3) / 4 * 4; }
};

uint32_t sector_bytes_for(const std::string& type) {
    if (type == "AUDIO") return 2352;
    if (type == "MODE1" || type == "MODE1/2048") return 2048;
    if (type == "MODE1_RAW" || type == "MODE1/2352" || type == "MODE2_RAW" || type == "MODE2/2352") return 2352;
    if (type == "MODE2" || type == "MODE2_FORM_MIX" || type == "MODE2/2336") return 2336;
    if (type == "MODE2_FORM1") return 2048;
    if (type == "MODE2_FORM2") return 2324;
    return 0;
}

std::map<std::string, std::string> fields(const std::string& s) {
    std::map<std::string, std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        const size_t colon = s.find(':', i);
        if (colon == std::string::npos)
            break;
        const size_t space = s.find(' ', colon);
        std::string key = s.substr(i, colon - i);
        std::string value = s.substr(colon + 1, space == std::string::npos ? std::string::npos : space - colon - 1);
        out[key] = value;
        if (space == std::string::npos)
            break;
        i = space + 1;
    }
    return out;
}

class Chd {
public:
    ~Chd() {
        if (chd_)
            chd_close(chd_);
    }
    Chd(const Chd&) = delete;
    Chd& operator=(const Chd&) = delete;
    Chd() = default;

    bool open(const fs::path& path, std::string& error) {
        const chd_error err = chd_open(path.string().c_str(), CHD_OPEN_READ, nullptr, &chd_);
        if (err != CHDERR_NONE) {
            error = std::string("cannot open CHD: ") + chd_error_string(err);
            return false;
        }
        header_ = chd_get_header(chd_);
        if (!header_ || header_->version != 5) {
            error = "only version 5 CHDs are supported";
            return false;
        }
        if (header_->unitbytes != kFrameBytes) {
            error = "this is not a CD/GD-ROM CHD (unit size " + std::to_string(header_->unitbytes) + " bytes)";
            return false;
        }
        if (header_->hunkbytes < kFrameBytes) {
            error = "the CHD's hunk size is too small";
            return false;
        }
        frames_per_hunk_ = header_->hunkbytes / kFrameBytes;
        hunk_.resize(header_->hunkbytes);
        tracks_ok_ = read_tracks(error);
        return tracks_ok_;
    }

    const std::vector<Track>& tracks() const { return tracks_; }
    const chd_header* header() const { return header_; }

    bool read_frame(uint64_t frame, std::vector<uint8_t>& out, std::string& error) {
        const uint32_t hunk = static_cast<uint32_t>(frame / frames_per_hunk_);
        if (hunk != cached_hunk_) {
            const chd_error err = chd_read(chd_, hunk, hunk_.data());
            if (err != CHDERR_NONE) {
                error = std::string("cannot read CHD hunk ") + std::to_string(hunk) + ": " + chd_error_string(err);
                return false;
            }
            cached_hunk_ = hunk;
        }
        const size_t offset = static_cast<size_t>(frame % frames_per_hunk_) * kFrameBytes;
        out.assign(hunk_.begin() + static_cast<std::ptrdiff_t>(offset),
                   hunk_.begin() + static_cast<std::ptrdiff_t>(offset + kFrameBytes));
        return true;
    }

private:
    bool add_metadata(uint32_t tag, std::string& error, int& found) {
        for (uint32_t index = 0;; ++index) {
            std::vector<uint8_t> data(4096);
            uint32_t length = 0, result_tag = 0;
            uint8_t flags = 0;
            const chd_error err = chd_get_metadata(chd_, tag, index, data.data(), static_cast<uint32_t>(data.size()),
                                                   &length, &result_tag, &flags);
            if (err == CHDERR_METADATA_NOT_FOUND)
                return true;
            if (err != CHDERR_NONE) {
                error = std::string("cannot read CHD metadata: ") + chd_error_string(err);
                return false;
            }
            if (length > data.size())
                length = static_cast<uint32_t>(data.size());
            std::string text(reinterpret_cast<const char*>(data.data()), length);
            while (!text.empty() && text.back() == '\0')
                text.pop_back();
            const auto kv = fields(text);
            auto it = kv.find("TRACK");
            if (it == kv.end())
                continue;
            Track t;
            t.number = std::atoi(it->second.c_str());
            if (auto v = kv.find("TYPE"); v != kv.end()) t.type = v->second;
            if (auto v = kv.find("FRAMES"); v != kv.end()) t.frames = std::strtoull(v->second.c_str(), nullptr, 10);
            if (auto v = kv.find("PAD"); v != kv.end()) t.pad = std::strtoull(v->second.c_str(), nullptr, 10);
            if (auto v = kv.find("PREGAP"); v != kv.end()) t.pregap = std::strtoull(v->second.c_str(), nullptr, 10);
            if (auto v = kv.find("PGTYPE"); v != kv.end()) t.pgtype = v->second;
            if (auto v = kv.find("POSTGAP"); v != kv.end()) t.postgap = std::strtoull(v->second.c_str(), nullptr, 10);
            t.sector_bytes = sector_bytes_for(t.type);
            t.audio = t.type == "AUDIO";
            if (t.sector_bytes == 0) {
                error = "unknown CHD track type " + t.type;
                return false;
            }
            tracks_.push_back(t);
            ++found;
        }
    }

    bool read_tracks(std::string& error) {
        int found = 0;
        for (uint32_t tag : {GDROM_TRACK_METADATA_TAG, GDROM_OLD_METADATA_TAG, CDROM_TRACK_METADATA2_TAG,
                             CDROM_TRACK_METADATA_TAG}) {
            if (!add_metadata(tag, error, found))
                return false;
        }
        if (tracks_.empty()) {
            error = "the CHD has no CD track metadata";
            return false;
        }
        std::sort(tracks_.begin(), tracks_.end(), [](const Track& a, const Track& b) { return a.number < b.number; });
        uint64_t chd_frame = 0, lba = 0;
        for (Track& t : tracks_) {
            const uint64_t stored = t.stored_pregap() ? t.pregap : 0;
            if (!t.stored_pregap())
                lba += t.pregap;
            t.chd_frame = chd_frame + stored;
            t.lba = lba;
            chd_frame += t.padded_frames();
            lba += t.frames + t.postgap;
        }
        return true;
    }

    chd_file* chd_ = nullptr;
    const chd_header* header_ = nullptr;
    std::vector<uint8_t> hunk_;
    uint32_t frames_per_hunk_ = 0;
    uint32_t cached_hunk_ = UINT32_MAX;
    std::vector<Track> tracks_;
    bool tracks_ok_ = false;
};

bool print_chd_info(const fs::path& path) {
    Chd chd;
    std::string error;
    if (!chd.open(path, error)) {
        std::cerr << "ikaruga-launcher: " << error << "\n";
        return false;
    }
    std::cout << path << "\n";
    std::cout << "  CHD v" << chd.header()->version << ", " << chd.header()->hunkbytes << "-byte hunks, "
              << (chd.header()->logicalbytes / 1024 / 1024) << " MiB\n";
    for (const Track& t : chd.tracks())
        std::cout << "  track " << t.number << ": " << t.type << ", LBA " << t.lba << ", " << t.sectors()
                  << " sectors" << (t.audio ? " (audio)" : "") << "\n";
    return true;
}

bool extract_chd(const fs::path& path, const fs::path& out_dir, fs::path& gdi, std::string& error) {
    Chd chd;
    if (!chd.open(path, error))
        return false;
    std::error_code ec;
    fs::create_directories(out_dir, ec);
    if (ec) {
        error = "cannot create " + out_dir.string() + ": " + ec.message();
        return false;
    }
    std::vector<std::string> names;
    uint64_t total = 0;
    for (const Track& t : chd.tracks()) {
        const std::string name = std::string("track") + (t.number < 10 ? "0" : "") + std::to_string(t.number) +
                                 (t.audio ? ".raw" : ".bin");
        names.push_back(name);
        std::ofstream out(out_dir / name, std::ios::binary);
        if (!out) {
            error = "cannot write " + (out_dir / name).string();
            return false;
        }
        std::cout << "  track " << t.number << ": " << t.type << ", " << t.sectors() << " sectors -> " << name
                  << std::endl;
        std::vector<uint8_t> frame;
        for (uint64_t i = 0; i < t.sectors(); ++i) {
            if (!chd.read_frame(t.chd_frame + i, frame, error))
                return false;
            if (t.audio) {
                // chdman stores CD audio big-endian; a GDI and the console's drive use little-endian.
                for (uint32_t b = 0; b + 1 < t.sector_bytes; b += 2)
                    std::swap(frame[b], frame[b + 1]);
            }
            out.write(reinterpret_cast<const char*>(frame.data()), t.sector_bytes);
            if (!out) {
                error = "cannot write " + (out_dir / name).string();
                return false;
            }
            total += t.sector_bytes;
        }
    }
    gdi = out_dir / "disc.gdi";
    std::ofstream out(gdi);
    if (!out) {
        error = "cannot write " + gdi.string();
        return false;
    }
    out << chd.tracks().size() << "\n";
    for (size_t i = 0; i < chd.tracks().size(); ++i) {
        const Track& t = chd.tracks()[i];
        out << t.number << " " << t.lba << " " << (t.audio ? 0 : 4) << " " << t.sector_bytes << " " << names[i]
            << " 0\n";
    }
    out.close();
    std::cout << "wrote " << gdi << " and " << total / 1024 / 1024 << " MiB of tracks\n";
    return true;
}

struct GdiTrack {
    int number = 0;
    uint32_t lba = 0;
    int type = 0;
    uint32_t sector_bytes = kSectorBytes;
    uint64_t offset = 0;
    std::ifstream file;
    std::string name;
};

class Gdi {
public:
    bool open(const fs::path& path, std::string& error) {
        std::ifstream in(path);
        if (!in) {
            error = "cannot read " + path.string();
            return false;
        }
        const fs::path dir = path.parent_path();
        int count = 0;
        in >> count;
        std::string line;
        std::getline(in, line);
        for (int i = 0; i < count && std::getline(in, line);) {
            std::istringstream row(line);
            GdiTrack t;
            if (!(row >> t.number >> t.lba >> t.type >> t.sector_bytes >> std::ws))
                continue;
            if (row.peek() == '"') {
                row.get();
                std::getline(row, t.name, '"');
            }
            else
                row >> t.name;
            uint64_t offset = 0;
            row >> offset;
            t.offset = offset;
            t.file.open(dir / t.name, std::ios::binary);
            if (!t.file) {
                error = "cannot read " + (dir / t.name).string();
                return false;
            }
            tracks_.push_back(std::move(t));
            ++i;
        }
        if (tracks_.empty()) {
            error = "the GDI has no tracks";
            return false;
        }
        std::sort(tracks_.begin(), tracks_.end(), [](const GdiTrack& a, const GdiTrack& b) { return a.lba < b.lba; });
        return true;
    }

    const std::vector<GdiTrack>& tracks() const { return tracks_; }

    bool read_sector(uint32_t lba, uint8_t (&out)[kDataBytes], std::string& error) {
        GdiTrack* track = nullptr;
        for (GdiTrack& t : tracks_)
            if (t.type == 4 && t.lba <= lba && (!track || t.lba > track->lba))
                track = &t;
        if (!track) {
            error = "LBA " + std::to_string(lba) + " is not in a data track";
            return false;
        }
        std::vector<uint8_t> raw(track->sector_bytes);
        track->file.clear();
        track->file.seekg(static_cast<std::streamoff>(track->offset + static_cast<uint64_t>(lba - track->lba) * track->sector_bytes));
        if (!track->file.read(reinterpret_cast<char*>(raw.data()), track->sector_bytes)) {
            error = "cannot read LBA " + std::to_string(lba);
            return false;
        }
        if (track->sector_bytes == kDataBytes) {
            std::memcpy(out, raw.data(), kDataBytes);
            return true;
        }
        if (track->sector_bytes < kSectorBytes) {
            error = "unsupported data sector size " + std::to_string(track->sector_bytes);
            return false;
        }
        const uint32_t skip = raw[15] == 2 ? 24 : 16;
        std::memcpy(out, raw.data() + skip, kDataBytes);
        return true;
    }

    uint32_t high_density_start() const {
        uint32_t start = 0;
        for (const GdiTrack& t : tracks_)
            if (t.type == 4 && t.lba >= 45000)
                start = t.lba;
        return start;
    }

private:
    std::vector<GdiTrack> tracks_;
};

struct Entry {
    std::string name;
    uint32_t lba = 0;
    uint32_t size = 0;
    bool directory = false;
};

Entry entry_of(const uint8_t* record) {
    Entry e;
    e.lba = le32(record + 2);
    e.size = le32(record + 10);
    e.directory = (record[25] & 2) != 0;
    const int length = record[32];
    for (int i = 0; i < length && record[33 + i] != ';'; ++i)
        e.name += static_cast<char>(record[33 + i]);
    return e;
}

bool read_root(Gdi& gdi, Entry& root, std::string& error) {
    const uint32_t start = gdi.high_density_start();
    if (!start) {
        error = "the GDI has no high-density data track";
        return false;
    }
    uint8_t pvd[kDataBytes];
    if (!gdi.read_sector(start + 16, pvd, error))
        return false;
    if (pvd[0] != 1 || std::memcmp(pvd + 1, "CD001", 5) != 0) {
        error = "the disc has no ISO 9660 primary volume descriptor";
        return false;
    }
    root = entry_of(pvd + 156);
    root.name.clear();
    return true;
}

std::vector<Entry> directory_entries(Gdi& gdi, const Entry& directory, std::string& error) {
    std::vector<Entry> out;
    uint8_t sector[kDataBytes];
    for (uint32_t done = 0; done < directory.size; done += kDataBytes) {
        if (!gdi.read_sector(directory.lba + done / kDataBytes, sector, error))
            return out;
        for (uint32_t at = 0; at < kDataBytes && sector[at] != 0; at += sector[at]) {
            const uint8_t* record = sector + at;
            Entry e = entry_of(record);
            if (e.name.size() <= 1)
                continue;  // "." and ".."
            out.push_back(std::move(e));
        }
    }
    return out;
}

bool extract_file(Gdi& gdi, const Entry& file, const fs::path& path, std::string& error) {
    std::error_code ec;
    if (!path.parent_path().empty())
        fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        error = "cannot write " + path.string();
        return false;
    }
    std::vector<uint8_t> data(file.size);
    uint8_t sector[kDataBytes];
    for (uint32_t done = 0; done < file.size; done += kDataBytes) {
        if (!gdi.read_sector(file.lba + done / kDataBytes, sector, error))
            return false;
        const uint32_t n = std::min<uint32_t>(kDataBytes, file.size - done);
        out.write(reinterpret_cast<const char*>(sector), n);
    }
    return static_cast<bool>(out);
}

bool extract_tree(Gdi& gdi, const Entry& directory, const fs::path& path, std::string& error, uint64_t& files,
                  uint64_t& bytes) {
    std::error_code ec;
    fs::create_directories(path, ec);
    for (const Entry& e : directory_entries(gdi, directory, error)) {
        if (e.name == "\0" || e.name.empty())
            continue;
        if (e.name.find('/') != std::string::npos || e.name.find('\\') != std::string::npos)
            continue;
        if (e.directory) {
            if (!extract_tree(gdi, e, path / e.name, error, files, bytes))
                return false;
        }
        else {
            if (!extract_file(gdi, e, path / e.name, error))
                return false;
            ++files;
            bytes += e.size;
        }
    }
    return true;
}

bool extract_disc(const fs::path& gdi_path, const fs::path& out_dir, std::string& error) {
    Gdi gdi;
    if (!gdi.open(gdi_path, error))
        return false;
    Entry root;
    if (!read_root(gdi, root, error))
        return false;
    std::error_code ec;
    fs::create_directories(out_dir, ec);
    const uint32_t start = gdi.high_density_start();
    std::ofstream boot(out_dir / "IP.BIN", std::ios::binary);
    if (boot) {
        uint8_t sector[kDataBytes];
        for (uint32_t i = 0; i < 16; ++i)
            if (gdi.read_sector(start + i, sector, error))
                boot.write(reinterpret_cast<const char*>(sector), sizeof sector);
    }
    uint64_t files = 0, bytes = 0;
    if (!extract_tree(gdi, root, out_dir / "fs", error, files, bytes))
        return false;
    std::cout << "extracted " << files << " files (" << bytes / 1024 / 1024 << " MiB) to " << out_dir << "\n";
    return true;
}

bool path_exists(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

std::vector<fs::path> find_discs() {
    std::vector<fs::path> out;
    std::error_code ec;
    for (const fs::path& dir : {fs::path("ROM"), fs::path("game/disc"), fs::path("game/prepared")}) {
        if (!fs::is_directory(dir, ec))
            continue;
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            if (!e.is_regular_file())
                continue;
            const std::string ext = extension(e.path());
            if (ext == ".gdi" || ext == ".chd")
                out.push_back(e.path());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

fs::path game_executable_in(const fs::path& here) {
    for (const fs::path& p : {here / "ikaruga", here / "ikaruga.exe",
                              here / "build-port" / "ikaruga", here / "build" / "ikaruga"})
        if (path_exists(p))
            return p;
    return {};
}

fs::path game_executable(const char* argv0) {
    std::error_code ec;
    return game_executable_in(fs::absolute(argv0, ec).parent_path());
}

}  // namespace

namespace launcher {

bool prepare(const PrepareRequest& request, PreparedDisc& prepared, std::string& error) {
    if (request.disc.empty()) {
        error = "no disc image selected";
        return false;
    }
    if (request.data_dir.empty()) {
        error = "no data folder selected";
        return false;
    }
    std::error_code ec;
    if (!fs::is_regular_file(request.disc, ec)) {
        error = "cannot read " + request.disc.string();
        return false;
    }
    const std::string ext = extension(request.disc);
    if (ext != ".gdi" && ext != ".chd") {
        error = request.disc.filename().string() + " is not a .gdi or .chd";
        return false;
    }

    fs::path gdi = request.disc;
    if (ext == ".chd") {
        const fs::path out = request.prepared_dir.empty()
                                 ? request.data_dir / "game" / "prepared" / request.disc.stem()
                                 : request.prepared_dir;
        gdi = out / "disc.gdi";
        bool cached = !request.rebuild && path_exists(gdi);
        if (cached) {
            Gdi check;
            std::string ignored;
            cached = check.open(gdi, ignored);
        }
        if (!cached) {
            std::cout << "unpacking " << request.disc << " to " << out << "\n";
            if (!extract_chd(request.disc, out, gdi, error))
                return false;
        }
    }
    else {
        Gdi check;
        if (!check.open(gdi, error))
            return false;
    }

    if (request.extract) {
        if (!extract_disc(gdi, request.data_dir / "game" / "extracted", error))
            return false;
    }

    prepared.gdi = fs::absolute(gdi, ec);
    if (ec)
        prepared.gdi = gdi;
    Gdi check;
    std::string ignored;
    if (check.open(prepared.gdi, ignored))
        prepared.description = "GDI · " + std::to_string(check.tracks().size()) + " tracks";
    else
        prepared.description = ext == ".chd" ? "CHD" : "GDI";
    return true;
}

std::filesystem::path find_game(const std::filesystem::path& where) { return game_executable_in(where); }

}  // namespace launcher

int run_cli(int argc, char** argv) {
    fs::path disc;
    fs::path out_dir;
    fs::path extract_dir = "game/extracted";
    bool do_extract = true, play = false, info = false;
    std::vector<std::string> game_args;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "ikaruga-launcher: " << a << " needs a value\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--disc") disc = next();
        else if (a == "--out") out_dir = next();
        else if (a == "--extract") extract_dir = next();
        else if (a == "--no-extract") do_extract = false;
        else if (a == "--play") play = true;
        else if (a == "--info") info = true;
        else if (a == "--") {
            for (++i; i < argc; ++i)
                game_args.emplace_back(argv[i]);
            break;
        }
        else if (a == "-h" || a == "--help") {
            usage();
            return 0;
        }
        else if (!a.empty() && a[0] == '-') {
            std::cerr << "ikaruga-launcher: unknown option " << a << "\n";
            return 2;
        }
        else if (disc.empty())
            disc = a;
        else {
            std::cerr << "ikaruga-launcher: more than one disc given\n";
            return 2;
        }
    }

    if (disc.empty()) {
        const auto discs = find_discs();
        if (discs.empty()) {
            std::cerr << "ikaruga-launcher: no .gdi or .chd in ROM/ or game/disc\n";
            usage();
            return 2;
        }
        std::cout << "Discs:\n";
        for (size_t i = 0; i < discs.size(); ++i)
            std::cout << "  " << (i + 1) << ". " << discs[i] << "\n";
        std::cout << "Choose one: " << std::flush;
        size_t choice = 0;
        if (!(std::cin >> choice) || choice == 0 || choice > discs.size()) {
            std::cerr << "ikaruga-launcher: no disc chosen\n";
            return 2;
        }
        disc = discs[choice - 1];
    }

    if (!path_exists(disc)) {
        std::cerr << "ikaruga-launcher: " << disc << " does not exist\n";
        return 2;
    }
    const std::string ext = extension(disc);
    if (info) {
        if (ext == ".chd")
            return print_chd_info(disc) ? 0 : 1;
        Gdi gdi;
        std::string error;
        if (!gdi.open(disc, error)) {
            std::cerr << "ikaruga-launcher: " << error << "\n";
            return 1;
        }
        std::cout << disc << "\n";
        for (const GdiTrack& t : gdi.tracks())
            std::cout << "  track " << t.number << ": LBA " << t.lba << ", " << t.sector_bytes << "-byte sectors\n";
        return 0;
    }

    fs::path gdi_path = disc;
    if (ext == ".chd") {
        if (out_dir.empty())
            out_dir = fs::path("game/prepared") / disc.stem();
        std::string error;
        std::cout << "unpacking " << disc << " to " << out_dir << "\n";
        if (!extract_chd(disc, out_dir, gdi_path, error)) {
            std::cerr << "ikaruga-launcher: " << error << "\n";
            return 1;
        }
    }
    else if (ext != ".gdi") {
        std::cerr << "ikaruga-launcher: " << disc << " is not a .gdi or .chd\n";
        return 2;
    }

    if (do_extract) {
        std::string error;
        if (!extract_disc(gdi_path, extract_dir, error)) {
            std::cerr << "ikaruga-launcher: " << error << "\n";
            return 1;
        }
    }

    std::cout << "disc ready: " << gdi_path << "\n";
    if (!play)
        return 0;

    const fs::path exe = game_executable(argv[0]);
    if (exe.empty()) {
        std::cerr << "ikaruga-launcher: ikaruga is not next to the launcher or in build-port/; run it with --disc "
                  << gdi_path << "\n";
        return 1;
    }
    std::string command = "\"" + exe.string() + "\" --disc \"" + gdi_path.string() + "\" --window";
    for (const std::string& arg : game_args)
        command += " \"" + arg + "\"";
#ifdef _WIN32
    // cmd.exe strips the first and last quote of a quoted executable path when it is the whole
    // command. An extra pair of quotes keeps the path intact for system().
    command = "\"" + command + "\"";
#endif
    std::cout << "starting " << command << "\n";
    return std::system(command.c_str());
}

int main(int argc, char** argv) {
    if (argc == 1)
        return launcher::run_gui();
    if (argc == 2) {
        const std::string ext = extension(argv[1]);
        if ((ext == ".gdi" || ext == ".chd") && path_exists(argv[1]))
            return launcher::run_gui(argv[1]);
    }
    return run_cli(argc, argv);
}

#ifdef _WIN32
// The launcher is a GUI program on Windows. A command-line invocation still needs a console for
// --info and --play diagnostics; attach to the parent console when there is one.
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    if (__argc > 1 && AttachConsole(ATTACH_PARENT_PROCESS)) {
        std::freopen("CONOUT$", "w", stdout);
        std::freopen("CONOUT$", "w", stderr);
    }
    return main(__argc, __argv);
}
#endif
