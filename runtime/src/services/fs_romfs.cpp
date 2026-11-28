// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from yuzu src/core/file_sys/fsmitm_romfsbuild.cpp (GPL-2.0-or-later; the table layout, entry sizes, the name hash and the
// bucket count rule) and ScarletNX src/nx/hle/filesystem/fs.c (GPL-3.0-or-later; the same rules, with unsigned name bytes).
// The image is laid out in memory: the header, the four tables and a map of segments. File data is read from the host files on demand.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "fs_romfs.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/diag.h"
#include "nxrt/log.h"
#include "nxrt/nup.h"
#include "nxrt/memory.h"
#include "nxrt/write_watch.h"

namespace nxrt::services::fs {
namespace {

constexpr uint32_t kNone = 0xFFFFFFFFu;     // "no entry" in every link field
constexpr uint64_t kHeaderSize = 0x50;      // ten u64 fields
constexpr uint64_t kPartitionOfs = 0x200;   // file data starts here; the header is padded to it
constexpr uint64_t kDataAlign = 0x10;       // each file starts at a multiple of this, counted from the partition start
constexpr uint64_t kDirFixed = 0x18;        // directory entry without its name
constexpr uint64_t kFileFixed = 0x20;       // file entry without its name
constexpr size_t kMaxName = 255;            // bytes of UTF-8 in one name
constexpr size_t kMaxPath = 0x300;          // SDK FS_MAX_PATH (0x301 with the terminator); the path counts its leading '/'
constexpr size_t kOpenFiles = 16;           // host files kept open by the cache
constexpr size_t kLongPath = 240;           // host paths this long or longer get the \\?\ prefix

uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }
uint64_t pad4(size_t n) { return align_up(n, 4); }

void put32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
void put64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

// Bucket hash of an entry named `name` under the directory entry at table offset `parent`. The name bytes count unsigned, as the
// console's char does (yuzu's signed char on x86 differs for non-ASCII names only).
uint32_t name_hash(uint32_t parent, const std::string& name) {
    uint32_t h = parent ^ 123456789u;
    for (unsigned char c : name) {
        h = (h >> 5) | (h << 27);
        h ^= c;
    }
    return h;
}

// Bucket count for n entries: 3 for fewer than 3, n|1 below 19, else the first number from n that no prime up to 17 divides.
uint64_t bucket_count(uint64_t n) {
    if (n < 3) return 3;
    if (n < 19) return n | 1;
    uint64_t c = n;
    while (c % 2 == 0 || c % 3 == 0 || c % 5 == 0 || c % 7 == 0 || c % 11 == 0 || c % 13 == 0 || c % 17 == 0) c++;
    return c;
}

bool to_utf8(const std::wstring& w, std::string& out) {
    out.clear();
    if (w.empty()) return true;
    const int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return false;
    out.resize(static_cast<size_t>(n));
    return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr) == n;
}

bool to_wide(const std::string& s, std::wstring& out) {
    out.clear();
    if (s.empty()) return false;
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return false;
    out.resize(static_cast<size_t>(n));
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), out.data(), n) == n;
}

// Host paths that may exceed MAX_PATH get the extended-length prefix. The input must already use backslashes.
std::wstring long_path(std::wstring p) {
    std::replace(p.begin(), p.end(), L'/', L'\\');
    if (p.size() < kLongPath || p.compare(0, 4, L"\\\\?\\") == 0) return p;
    if (p.compare(0, 2, L"\\\\") == 0) return L"\\\\?\\UNC\\" + p.substr(2);
    return L"\\\\?\\" + p;
}

// One folder or file of the host tree. Layout fields are filled in once the tree is complete.
struct Node {
    std::string name;            // UTF-8; empty for the root
    std::string path;            // image path: "" for the root, "/a/b" otherwise
    std::wstring host;           // host path to open
    bool is_dir = false;
    uint64_t size = 0;           // files only
    uint64_t src = 0;            // files: offset of the data in the host file (0, or the extent in a .nup)
    std::vector<Node> kids;      // sorted by name bytes
    uint32_t entry = 0;          // offset of this entry in its table
    uint32_t parent = 0;         // entry offset of the parent directory (the root's is 0)
    uint32_t sibling = kNone;    // next entry in the parent's chain of the same kind
    uint32_t first_child = kNone;  // directories: first subdirectory entry
    uint32_t first_file = kNone;   // directories: first file entry
    uint64_t data = 0;           // files: offset of the data from the partition start
    std::shared_ptr<const std::vector<uint8_t>> mem;   // files: the content after an edit (RomfsEdits), served instead of the host file
    bool from_mod = false;       // files: taken from a RomFS mod (merge), to name the clash when a second mod has it too
};

std::string shown(const Node& n) { return n.path.empty() ? std::string("/") : n.path; }

// Fills dir.kids from the host folder dir.host, recursively. False with msg set at the first entry that cannot be described.
bool scan(Node& dir, std::string& msg) {
    std::error_code ec;
    std::filesystem::directory_iterator it(std::filesystem::path(dir.host), ec);
    if (ec) {
        msg = "cannot read folder " + shown(dir);
        return false;
    }
    const std::filesystem::directory_iterator end;
    for (;;) {
        if (ec) {
            msg = "cannot read folder " + shown(dir);
            return false;
        }
        if (it == end) break;
        const std::filesystem::directory_entry& e = *it;
        std::string name;
        if (!to_utf8(e.path().filename().wstring(), name) || name.empty() || name.size() > kMaxName) {
            msg = "name cannot be stored in a RomFS (1 to 255 bytes of UTF-8): " + shown(dir) + "/" + name;
            return false;
        }
        Node k;
        k.name = name;
        k.path = dir.path + "/" + name;
        if (k.path.size() > kMaxPath) {
            msg = "path too long for a RomFS (0x300 bytes): " + k.path;
            return false;
        }
        k.host = long_path(e.path().wstring());
        std::error_code sec;
        const std::filesystem::file_status st = e.symlink_status(sec);
        if (sec) {
            msg = "cannot stat " + k.path;
            return false;
        }
        if (std::filesystem::is_symlink(st)) {
            msg = "symbolic link cannot be described: " + k.path;
            return false;
        }
        if (std::filesystem::is_directory(st)) {
            k.is_dir = true;
            if (!scan(k, msg)) return false;
        } else if (std::filesystem::is_regular_file(st)) {
            k.size = e.file_size(sec);
            if (sec) {
                msg = "cannot read the size of " + k.path;
                return false;
            }
        } else {
            msg = "not a file or folder: " + k.path;
            return false;
        }
        dir.kids.push_back(std::move(k));
        it.increment(ec);
    }
    // std::string's operator< compares unsigned char values, which is the byte order the RomFS hash tables expect.
    std::sort(dir.kids.begin(), dir.kids.end(), [](const Node& a, const Node& b) { return a.name < b.name; });
    return true;
}

// [fs] scan_threads (C1, startup): the same tree as scan, read with FindFirstFileExW (FindExInfoBasic, large fetches: the name, attributes and
// size come with each entry, no stat per file), every folder a task for a few threads. Fortune's Weave's RomFS is ~252,000 files in ~21,000
// folders, and scan took ~4.5 s of every start on the laptop (the main thread waits for it at the game's first file access). Same checks and
// messages as scan: a symbolic link and a junction are refused as there; other reparse points (compressed files of `compact /exe` or
// CompactGUI, deduplicated or cloud files) are plain files and folders, as std::filesystem treats them.
struct FastScan {
    std::mutex mu;
    std::condition_variable cv;
    std::vector<Node*> work;      // folders still to read
    int busy = 0;                 // threads reading a folder now (under mu)
    std::string msg;              // the first error
    bool failed = false;
};

// dir.host without a trailing separator (a folder named "game/romfs/" in the ini), so that a child's path has one; a drive root keeps it.
std::wstring host_base(const std::wstring& h) {
    std::wstring b = h;
    while (b.size() > 1 && (b.back() == L'\\' || b.back() == L'/') && !(b.size() >= 2 && b[b.size() - 2] == L':')) b.pop_back();
    return b;
}

// Reads one folder's entries into dir.kids (subfolders are left empty and returned in subdirs).
bool scan_one(Node& dir, std::vector<Node*>& subdirs, std::string& msg) {
    const std::wstring base = host_base(dir.host);
    const bool sep = !base.empty() && base.back() == L'\\';
    WIN32_FIND_DATAW fd;
    const std::wstring pat = base + (sep ? L"*" : L"\\*");
    HANDLE h = FindFirstFileExW(pat.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND) return true;   // an empty folder (no "." entry on some file systems)
        msg = "cannot read folder " + shown(dir);
        return false;
    }
    bool ok = true;
    do {
        const wchar_t* w = fd.cFileName;
        if ((w[0] == L'.' && w[1] == 0) || (w[0] == L'.' && w[1] == L'.' && w[2] == 0)) continue;
        std::string name;
        if (!to_utf8(std::wstring(w), name) || name.empty() || name.size() > kMaxName) {
            msg = "name cannot be stored in a RomFS (1 to 255 bytes of UTF-8): " + shown(dir) + "/" + name;
            ok = false;
            break;
        }
        Node k;
        k.name = name;
        k.path = dir.path + "/" + name;
        if (k.path.size() > kMaxPath) {
            msg = "path too long for a RomFS (0x300 bytes): " + k.path;
            ok = false;
            break;
        }
        k.host = long_path(base + (sep ? L"" : L"\\") + w);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            // dwReserved0 is the reparse tag here (as std::filesystem's symlink_status reads it)
            if (fd.dwReserved0 == IO_REPARSE_TAG_SYMLINK) {
                msg = "symbolic link cannot be described: " + k.path;
                ok = false;
                break;
            }
            if (fd.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT) {
                msg = "not a file or folder: " + k.path;
                ok = false;
                break;
            }
        }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            k.is_dir = true;
        } else {
            k.size = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        }
        dir.kids.push_back(std::move(k));
    } while (FindNextFileW(h, &fd));
    if (ok && GetLastError() != ERROR_NO_MORE_FILES) {
        msg = "cannot read folder " + shown(dir);
        ok = false;
    }
    FindClose(h);
    if (!ok) return false;
    std::sort(dir.kids.begin(), dir.kids.end(), [](const Node& a, const Node& b) { return a.name < b.name; });
    for (Node& k : dir.kids)       // pointers into kids stay valid: the vector is complete and never grows again
        if (k.is_dir) subdirs.push_back(&k);
    return true;
}

bool scan_parallel(Node& root, int threads, std::string& msg) {
    FastScan st;
    st.work.push_back(&root);
    auto worker = [&] {
        std::vector<Node*> sub;
        for (;;) {
            Node* d = nullptr;
            {
                std::unique_lock lk(st.mu);
                // wait for a folder, or for the end: nothing queued and nobody reading (no more can come), or an error
                st.cv.wait(lk, [&] { return st.failed || !st.work.empty() || st.busy == 0; });
                if (st.failed || st.work.empty()) return;
                d = st.work.back();
                st.work.pop_back();
                st.busy++;
            }
            sub.clear();
            std::string m;
            const bool ok = scan_one(*d, sub, m);
            {
                std::lock_guard lk(st.mu);
                if (!ok && !st.failed) {
                    st.failed = true;
                    st.msg = m;
                }
                st.work.insert(st.work.end(), sub.begin(), sub.end());
                st.busy--;
            }
            st.cv.notify_all();
        }
    };
    std::vector<std::thread> pool;
    for (int i = 1; i < threads; ++i) pool.emplace_back(worker);
    worker();
    for (auto& t : pool) t.join();
    if (st.failed) {
        msg = st.msg;
        return false;
    }
    return true;
}

int scan_threads() {
    static const int n = [] {
        const unsigned hw = std::thread::hardware_concurrency();
        const int fallback = static_cast<int>(std::clamp<unsigned>(hw / 2, 1u, 8u));
        std::string v = nxrt::config_get_str("fs", "scan_threads", "auto");
        for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (v.empty() || v == "auto") return fallback;
        char* end = nullptr;
        const long x = std::strtol(v.c_str(), &end, 10);
        if (!end || *end || x < 0) {
            NXLOG_W("fs", "[fs] scan_threads = '%s' is not a number or auto: auto (%d threads) is used", v.c_str(), fallback);
            return fallback;
        }
        return static_cast<int>(std::min<long>(x, 32));
    }();
    return n;
}

// The host tree into dir: scan_parallel, or scan with [fs] scan_threads = 0. Logs how long it took and for which folder.
bool scan_tree(Node& dir, std::string& msg) {
    const auto t0 = std::chrono::steady_clock::now();
    const int n = scan_threads();
    const bool ok = n > 0 ? scan_parallel(dir, n, msg) : scan(dir, msg);
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::string where;
    if (!to_utf8(dir.host, where)) where = "?";
    NXLOG_I("fs", "romfs: folder %s read in %.2f s (%s)", where.c_str(), s,
            n > 0 ? (std::to_string(n) + " threads, [fs] scan_threads").c_str() : "one thread, [fs] scan_threads = 0");
    return ok;
}

// ---- [fs] prescan --------------------------------------------------------------------------------------------------------------
// Trees read ahead on a background thread (romfs_prescan), by the host path build() gives its root Node (long_path of the absolute
// path). Never freed: the thread may still be reading when the process ends.
struct Prescanned {
    bool done = false;
    bool ok = false;
    Node node;
    std::string msg;
    double seconds = 0;
};
struct PrescanState {
    std::mutex mu;
    std::condition_variable cv;
    std::map<std::wstring, Prescanned> trees;
    std::atomic<uint32_t> taken{0};
    std::atomic<bool> started{false};
};
PrescanState& prescan_state() {
    static PrescanState* s = new PrescanState();
    return *s;
}

// The tree of dir.host: the prescanned one when romfs_prescan read (or is reading) it, else scan_tree.
bool scan_or_prescanned(Node& dir, std::string& msg) {
    PrescanState& ps = prescan_state();
    if (ps.started.load(std::memory_order_acquire)) {
        std::unique_lock<std::mutex> lk(ps.mu);
        auto it = ps.trees.find(dir.host);
        if (it != ps.trees.end()) {
            const auto t0 = std::chrono::steady_clock::now();
            ps.cv.wait(lk, [&] { return it->second.done; });
            const double waited = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            Prescanned p = std::move(it->second);
            ps.trees.erase(it);
            lk.unlock();
            ps.taken.fetch_add(1, std::memory_order_relaxed);
            std::string where;
            if (!to_utf8(dir.host, where)) where = "?";
            NXLOG_I("fs", "romfs: folder %s from the start-up read ([fs] prescan: read in %.2f s on its own thread, waited %.2f s)", where.c_str(),
                    p.seconds, waited);
            if (!p.ok) {
                msg = p.msg;
                return false;
            }
            p.node.name = dir.name;
            p.node.path = dir.path;
            dir = std::move(p.node);
            return true;
        }
    }
    return scan_tree(dir, msg);
}

void sort_tree(Node& d) {
    std::sort(d.kids.begin(), d.kids.end(), [](const Node& a, const Node& b) { return a.name < b.name; });
    for (Node& k : d.kids)
        if (k.is_dir) sort_tree(k);
}

// Fills root from the RomFS of a .nup package: every file is a range of the package file.
bool scan_nup(const std::string& nup_path, Node& root, std::string& msg) {
    nup::Content c;
    if (!nup::open(nup_path, c, &msg)) return false;
    if (c.romfs.empty()) {
        msg = nup_path + " has no RomFS";
        return false;
    }
    for (const nup::File& f : c.romfs) {
        Node* d = &root;
        size_t at = 0;
        for (size_t slash; (slash = f.path.find('/', at)) != std::string::npos; at = slash + 1) {
            const std::string name = f.path.substr(at, slash - at);
            auto it = std::find_if(d->kids.begin(), d->kids.end(), [&](const Node& k) { return k.is_dir && k.name == name; });
            if (it == d->kids.end()) {
                Node k;
                k.is_dir = true;
                k.name = name;
                k.path = d->path + "/" + name;
                d->kids.push_back(std::move(k));
                it = d->kids.end() - 1;
            }
            d = &*it;
        }
        Node k;
        k.name = f.path.substr(at);
        k.path = "/" + f.path;
        k.host = root.host;
        k.size = f.size;
        k.src = f.offset;
        if (k.name.size() > kMaxName || k.path.size() > kMaxPath) {
            msg = "name or path too long for a RomFS: " + k.path;
            return false;
        }
        d->kids.push_back(std::move(k));
    }
    sort_tree(root);
    return true;
}

// ---- overlays (RomFS mods) --------------------------------------------------------------------------------------------------------
bool same_name_nocase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        unsigned char x = static_cast<unsigned char>(a[i]), y = static_cast<unsigned char>(b[i]);
        if (x >= 'A' && x <= 'Z') x = static_cast<unsigned char>(x + 32);
        if (y >= 'A' && y <= 'Z') y = static_cast<unsigned char>(y + 32);
        if (x != y) return false;
    }
    return true;
}

uint32_t count_files(const Node& n) {
    if (!n.is_dir) return 1;
    uint32_t c = 0;
    for (const Node& k : n.kids) c += count_files(k);
    return c;
}

// Gives n (and everything under it) its image path under the directory `parent`. False when a path gets too long.
bool repath(Node& n, const std::string& parent, std::string& msg) {
    n.path = parent + "/" + n.name;
    if (n.path.size() > kMaxPath) {
        msg = "path too long for a RomFS (0x300 bytes): " + n.path;
        return false;
    }
    for (Node& k : n.kids)
        if (!repath(k, n.path, msg)) return false;
    return true;
}

thread_local uint32_t t_clashes = 0;   // files found in two RomFS mods in the layout being built (make_layout resets and reports it)

void mark_from_mod(Node& n) {
    n.from_mod = !n.is_dir;
    for (Node& k : n.kids) mark_from_mod(k);
}

// Lays the overlay directory `add` over `base`. `taken` counts the files taken from the overlay.
bool merge(Node& base, Node& add, uint32_t& taken, std::string& msg) {
    for (Node& k : add.kids) {
        auto it = std::find_if(base.kids.begin(), base.kids.end(), [&](const Node& b) { return b.name == k.name; });
        if (it == base.kids.end())
            it = std::find_if(base.kids.begin(), base.kids.end(), [&](const Node& b) { return same_name_nocase(b.name, k.name); });
        if (it == base.kids.end()) {
            if (!repath(k, base.path, msg)) return false;
            mark_from_mod(k);
            taken += count_files(k);
            base.kids.push_back(std::move(k));
        } else if (it->is_dir && k.is_dir) {
            if (!merge(*it, k, taken, msg)) return false;
        } else if (!it->is_dir && !k.is_dir) {
            if (it->from_mod && ++t_clashes <= 50) {   // one line per file for the first 50, the count at the end (make_layout)
                std::string from;
                if (!to_utf8(k.host, from)) from = "?";
                NXLOG_I("mods", "romfs: %s is in two mods: %s is used ([fs] mod_priority)", it->path.c_str(), from.c_str());
            }
            it->host = k.host;
            it->from_mod = true;
            it->size = k.size;
            it->src = 0;
            taken++;
        } else {
            // a file where the game has a folder, or the other way round: the overlay's entry replaces the game's
            const std::string name = it->name;
            *it = std::move(k);
            mark_from_mod(*it);
            it->name = name;
            if (!repath(*it, base.path, msg)) return false;
            taken += count_files(*it);
        }
    }
    return true;
}

void flatten(Node& n, std::vector<Node*>& dirs, std::vector<Node*>& files) {
    for (Node& k : n.kids) {
        if (k.is_dir) {
            dirs.push_back(&k);
            flatten(k, dirs, files);
        } else {
            files.push_back(&k);
        }
    }
}

struct Segment {
    enum class Kind : uint8_t { kHeader, kTables, kZero, kFile, kMem };
    uint64_t off = 0;            // image offset
    uint64_t len = 0;
    Kind kind = Kind::kZero;
    uint64_t src = 0;            // kHeader/kTables: offset in that buffer; kFile: offset in the host file
    uint32_t file = 0;           // kFile, kMem: index into Layout::files
};

struct FileRec {
    std::wstring host;
    uint64_t size = 0;
    uint64_t image_off = 0;
    uint64_t src = 0;            // offset of the data in the host file
    std::string path;            // path inside the image ("/a/b.bin"), for the first-read note
    std::shared_ptr<const std::vector<uint8_t>> mem;   // set: the content is here (an edited file), the host file is not read
    int16_t overlay = -1;        // the RomFS mod the file comes from (index into Layout::overlay_names), -1: the game's
};

// The whole of a file of the tree (a host file, or its range of a package). False when it cannot be read.
bool read_whole(const Node& f, std::vector<uint8_t>& out) {
    HANDLE h = CreateFileW(f.host.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    out.resize(static_cast<size_t>(f.size));
    uint64_t done = 0;
    bool ok = true;
    while (ok && done < f.size) {
        const DWORD chunk = static_cast<DWORD>(std::min<uint64_t>(f.size - done, 1ull << 30));
        const uint64_t ofs = f.src + done;
        OVERLAPPED ov{};
        ov.Offset = static_cast<DWORD>(ofs & 0xFFFFFFFFull);
        ov.OffsetHigh = static_cast<DWORD>(ofs >> 32);
        DWORD got = 0;
        ok = ReadFile(h, out.data() + done, chunk, &got, &ov) && got == chunk;
        done += chunk;
    }
    CloseHandle(h);
    return ok;
}

// Everything build() produces. Replaced as a whole.
struct Layout {
    bool built = false;
    uint64_t size = 0;
    RomfsImage::Stats stats;
    std::array<uint8_t, kHeaderSize> header{};
    std::vector<uint8_t> tables;        // dir hash, dir table, file hash, file table, in that order from dir_hash_table_ofs
    std::vector<Segment> segs;          // contiguous, covering [0, size)
    std::vector<FileRec> files;         // in data order
    std::map<std::string, uint32_t> by_path;
    std::vector<std::string> overlay_names;   // per applied RomFS mod: its name (the folder holding its romfs folder), for the reports
};

// Lays out the host folder: entries sorted by full path in each table, file data in that order, 16-byte aligned.
bool make_layout(const std::string& host_dir, const std::vector<std::string>& overlays, const RomfsEdits* edits, Layout& L,
                 std::string* err) {
    std::string msg;
    std::wstring wdir;
    const bool packaged = nup::is_nup(host_dir);
    if (!to_wide(host_dir, wdir)) {
        if (err) *err = "host folder name is empty or not valid UTF-8";
        return false;
    }
    std::error_code ec;
    const std::filesystem::path abs = std::filesystem::absolute(std::filesystem::path(wdir), ec);
    if (ec || (!packaged && !std::filesystem::is_directory(abs, ec))) {
        if (err) *err = "not a folder: " + host_dir;
        return false;
    }
    Node root;
    root.is_dir = true;
    root.host = long_path(abs.wstring());
    if (!(packaged ? scan_nup(host_dir, root, msg) : scan_or_prescanned(root, msg))) {
        if (err) *err = msg;
        return false;
    }
    uint32_t taken = 0;
    t_clashes = 0;
    std::vector<std::wstring> overlay_hosts;   // the applied overlays' folders, parallel to L.overlay_names
    for (const std::string& o : overlays) {
        std::wstring wo;
        std::error_code oec;
        const std::filesystem::path oabs = to_wide(o, wo) ? std::filesystem::absolute(std::filesystem::path(wo), oec) : std::filesystem::path();
        if (oabs.empty() || oec || !std::filesystem::is_directory(oabs, oec)) {
            NXLOG_W("fs", "romfs overlay '%s' is not a folder: left out", o.c_str());
            continue;
        }
        Node ov;
        ov.is_dir = true;
        ov.host = long_path(oabs.wstring());
        std::string omsg;
        if (!scan_or_prescanned(ov, omsg)) {
            NXLOG_W("fs", "romfs overlay '%s' left out: %s", o.c_str(), omsg.c_str());
            continue;
        }
        // merge into a copy, so an overlay that fails halfway (a path too long) leaves the tree as it was
        Node merged = root;
        uint32_t n = 0;
        if (!merge(merged, ov, n, omsg)) {
            NXLOG_W("fs", "romfs overlay '%s' left out: %s", o.c_str(), omsg.c_str());
            continue;
        }
        root = std::move(merged);
        taken += n;
        NXLOG_I("fs", "romfs overlay '%s': %u files", o.c_str(), n);
        {   // the mod's name: "mods/<name>/romfs" gives <name>, any other folder its own name
            std::filesystem::path q = oabs;
            if (!q.has_filename()) q = q.parent_path();
            std::string leaf;
            const bool is_romfs = to_utf8(q.filename().wstring(), leaf) && same_name_nocase(leaf, "romfs");
            const std::filesystem::path named = is_romfs && q.has_parent_path() ? q.parent_path().filename() : q.filename();
            std::string name;
            if (!to_utf8(named.wstring(), name) || name.empty()) name = o;
            L.overlay_names.push_back(name);
            overlay_hosts.push_back(ov.host);
        }
    }
    if (!overlays.empty()) sort_tree(root);

    std::vector<Node*> dirs{&root}, files;
    flatten(root, dirs, files);
    auto by_path = [](const Node* a, const Node* b) { return a->path < b->path; };
    std::sort(dirs.begin(), dirs.end(), by_path);
    std::sort(files.begin(), files.end(), by_path);

    // Edits by code mods: the file as it stands after the overlays, changed in memory. Its size is the edited one from here on.
    uint32_t edited = 0;
    if (edits && edits->wants && edits->edit) {
        for (Node* f : files) {
            if (!edits->wants(f->path)) continue;
            if (f->size > edits->max_size) {
                NXLOG_W("fs", "romfs: %s is too large to be edited by a mod (%llu bytes): served as it is", f->path.c_str(),
                        static_cast<unsigned long long>(f->size));
                continue;
            }
            std::vector<uint8_t> data;
            if (!read_whole(*f, data)) {
                NXLOG_W("fs", "romfs: %s cannot be read for a mod's edit: served as it is", f->path.c_str());
                continue;
            }
            const uint64_t before = f->size;
            if (!edits->edit(f->path, data)) continue;
            f->size = data.size();
            f->mem = std::make_shared<const std::vector<uint8_t>>(std::move(data));
            edited++;
            NXLOG_I("fs", "romfs: %s edited by a code mod: %llu -> %llu bytes", f->path.c_str(), static_cast<unsigned long long>(before),
                    static_cast<unsigned long long>(f->size));
        }
    }

    // Entry offsets in the two tables (root first: its path is empty).
    uint64_t dir_bytes = 0, file_bytes = 0;
    for (Node* d : dirs) {
        d->entry = static_cast<uint32_t>(dir_bytes);
        dir_bytes += kDirFixed + pad4(d->name.size());
    }
    for (Node* f : files) {
        f->entry = static_cast<uint32_t>(file_bytes);
        file_bytes += kFileFixed + pad4(f->name.size());
    }
    if (dir_bytes >= 0xFFFFFFF0ull || file_bytes >= 0xFFFFFFF0ull) {
        if (err) *err = "too many entries for a RomFS";
        return false;
    }

    // File data: each file at a multiple of 16 from the partition start.
    uint64_t part = 0;
    for (Node* f : files) {
        part = align_up(part, kDataAlign);
        f->data = part;
        part += f->size;
    }

    // Links: each directory's children in name order, as chains of sibling entries.
    for (Node* d : dirs) {
        std::vector<Node*> sub, fil;
        for (Node& k : d->kids) (k.is_dir ? sub : fil).push_back(&k);
        d->first_child = sub.empty() ? kNone : sub.front()->entry;
        d->first_file = fil.empty() ? kNone : fil.front()->entry;
        for (size_t i = 0; i < sub.size(); i++) {
            sub[i]->parent = d->entry;
            sub[i]->sibling = i + 1 < sub.size() ? sub[i + 1]->entry : kNone;
        }
        for (size_t i = 0; i < fil.size(); i++) {
            fil[i]->parent = d->entry;
            fil[i]->sibling = i + 1 < fil.size() ? fil[i + 1]->entry : kNone;
        }
    }

    // Table placement: the header is 0x50 bytes, the partition starts at 0x200, then the four tables, each 4-byte aligned.
    const uint64_t dbuckets = bucket_count(dirs.size());
    const uint64_t fbuckets = bucket_count(files.size());
    const uint64_t dhash_size = 4 * dbuckets, fhash_size = 4 * fbuckets;
    const uint64_t dhash_ofs = align_up(kPartitionOfs + part, 4);
    const uint64_t dtab_ofs = dhash_ofs + dhash_size;
    const uint64_t fhash_ofs = dtab_ofs + dir_bytes;
    const uint64_t ftab_ofs = fhash_ofs + fhash_size;
    const uint64_t image_size = ftab_ofs + file_bytes;

    L.tables.assign(dhash_size + dir_bytes + fhash_size + file_bytes, 0);
    uint8_t* dh = L.tables.data();
    uint8_t* dt = dh + dhash_size;
    uint8_t* fh = dt + dir_bytes;
    uint8_t* ft = fh + fhash_size;
    std::vector<uint32_t> dhead(dbuckets, kNone), fhead(fbuckets, kNone);

    for (Node* d : dirs) {
        const uint32_t b = static_cast<uint32_t>(name_hash(d->parent, d->name) % dbuckets);
        uint8_t* e = dt + d->entry;
        put32(e + 0, d->parent);
        put32(e + 4, d->sibling);
        put32(e + 8, d->first_child);
        put32(e + 12, d->first_file);
        put32(e + 16, dhead[b]);
        put32(e + 20, static_cast<uint32_t>(d->name.size()));
        std::memcpy(e + kDirFixed, d->name.data(), d->name.size());
        dhead[b] = d->entry;
    }
    for (Node* f : files) {
        const uint32_t b = static_cast<uint32_t>(name_hash(f->parent, f->name) % fbuckets);
        uint8_t* e = ft + f->entry;
        put32(e + 0, f->parent);
        put32(e + 4, f->sibling);
        put64(e + 8, f->data);
        put64(e + 16, f->size);
        put32(e + 24, fhead[b]);
        put32(e + 28, static_cast<uint32_t>(f->name.size()));
        std::memcpy(e + kFileFixed, f->name.data(), f->name.size());
        fhead[b] = f->entry;
    }
    for (size_t i = 0; i < dhead.size(); i++) put32(dh + 4 * i, dhead[i]);
    for (size_t i = 0; i < fhead.size(); i++) put32(fh + 4 * i, fhead[i]);

    std::memset(L.header.data(), 0, L.header.size());
    put64(L.header.data() + 0, kHeaderSize);
    put64(L.header.data() + 8, dhash_ofs);
    put64(L.header.data() + 16, dhash_size);
    put64(L.header.data() + 24, dtab_ofs);
    put64(L.header.data() + 32, dir_bytes);
    put64(L.header.data() + 40, fhash_ofs);
    put64(L.header.data() + 48, fhash_size);
    put64(L.header.data() + 56, ftab_ofs);
    put64(L.header.data() + 64, file_bytes);
    put64(L.header.data() + 72, kPartitionOfs);

    // Segments: header, padding, then each file's data with the zero gaps between, the zero gap up to the tables, the tables.
    L.segs.clear();
    L.files.clear();
    L.by_path.clear();
    L.segs.push_back({0, kHeaderSize, Segment::Kind::kHeader, 0, 0});
    L.segs.push_back({kHeaderSize, kPartitionOfs - kHeaderSize, Segment::Kind::kZero, 0, 0});
    uint64_t cursor = kPartitionOfs;
    uint64_t data_bytes = 0;
    for (Node* f : files) {
        const uint64_t img = kPartitionOfs + f->data;
        if (img > cursor) L.segs.push_back({cursor, img - cursor, Segment::Kind::kZero, 0, 0});
        const uint32_t idx = static_cast<uint32_t>(L.files.size());
        L.files.push_back({f->host, f->size, img, f->src, f->path, f->mem});
        // a file from a RomFS mod: its host path lies in that mod's folder (the last mod laid down wins, as in merge)
        for (size_t o = overlay_hosts.size(); o-- > 0;) {
            const std::wstring& oh = overlay_hosts[o];
            if (f->host.size() > oh.size() && f->host.compare(0, oh.size(), oh) == 0 && (f->host[oh.size()] == L'\\' || f->host[oh.size()] == L'/')) {
                L.files.back().overlay = static_cast<int16_t>(o);
                break;
            }
        }
        if (f->size > 0) L.segs.push_back({img, f->size, f->mem ? Segment::Kind::kMem : Segment::Kind::kFile, f->mem ? 0 : f->src, idx});
        L.by_path[f->path] = idx;
        cursor = img + f->size;
        data_bytes += f->size;
    }
    if (dhash_ofs > cursor) L.segs.push_back({cursor, dhash_ofs - cursor, Segment::Kind::kZero, 0, 0});
    L.segs.push_back({dhash_ofs, image_size - dhash_ofs, Segment::Kind::kTables, 0, 0});

    L.size = image_size;
    L.stats.files = static_cast<uint32_t>(files.size());
    L.stats.dirs = static_cast<uint32_t>(dirs.size());
    L.stats.data_bytes = data_bytes;
    L.stats.data_offset = kPartitionOfs;
    L.stats.overlay_files = taken;
    if (t_clashes > 50) NXLOG_I("mods", "romfs: %u files in all are in two mods (the first 50 are named above)", t_clashes);
    L.stats.edited_files = edited;
    L.built = true;
    return true;
}

// One host file kept open. The box closes its handle when the last reader and the cache have let go of it.
struct HandleBox {
    HANDLE h = INVALID_HANDLE_VALUE;
    HandleBox() = default;
    HandleBox(const HandleBox&) = delete;
    HandleBox& operator=(const HandleBox&) = delete;
    ~HandleBox() {
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
};

struct OpenSlot {
    uint32_t file = kNone;
    uint64_t used = 0;
    std::shared_ptr<HandleBox> box;
};

}  // namespace

struct RomfsImage::Impl {
    Layout l;
    mutable std::mutex mu;                  // guards cache, tick and read_seen
    mutable std::vector<uint8_t> read_seen; // per file: read at least once (the game's loads, in the log and the crash report)
    void note_read(uint32_t idx) const {
        {
            std::lock_guard<std::mutex> lk(mu);
            if (idx >= read_seen.size() || read_seen[idx]) return;
            read_seen[idx] = 1;
        }
        const FileRec& fr = l.files[idx];
        NXLOG_D("fs", "romfs: first read of %s", fr.path.c_str());
        // the crash and device-lost reports list these: a file from a mod, or changed by a code mod, says so (#121)
        if (fr.overlay >= 0 && static_cast<size_t>(fr.overlay) < l.overlay_names.size())
            diag_note_file("rom", (fr.path + "  [mod: " + l.overlay_names[static_cast<size_t>(fr.overlay)] + (fr.mem ? ", edited by a code mod]" : "]")).c_str());
        else if (fr.mem)
            diag_note_file("rom", (fr.path + "  [edited by a code mod]").c_str());
        else
            diag_note_file("rom", fr.path.c_str());
    }
    mutable std::vector<OpenSlot> cache;    // at most kOpenFiles entries
    mutable uint64_t tick = 0;

    // The handle for file idx, opened if it is not cached. The least recently used entry is dropped when the cache is full.
    std::shared_ptr<HandleBox> acquire(uint32_t idx) const {
        std::lock_guard<std::mutex> lk(mu);
        for (OpenSlot& s : cache) {
            if (s.file == idx) {
                s.used = ++tick;
                return s.box;
            }
        }
        const FileRec& f = l.files[idx];
        HANDLE h = CreateFileW(f.host.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return nullptr;
        auto box = std::make_shared<HandleBox>();
        box->h = h;
        if (cache.size() < kOpenFiles) {
            cache.push_back(OpenSlot{idx, ++tick, box});
        } else {
            auto victim = std::min_element(cache.begin(), cache.end(),
                                           [](const OpenSlot& a, const OpenSlot& b) { return a.used < b.used; });
            *victim = OpenSlot{idx, ++tick, box};
        }
        return box;
    }

    // Positioned reads (OVERLAPPED offset), so concurrent readers of one handle need no seek lock.
    bool read_host(uint32_t idx, uint64_t ofs, uint8_t* out, uint64_t n) const {
        std::shared_ptr<HandleBox> box = acquire(idx);
        if (!box) return false;
        mem::heap_touch(reinterpret_cast<uint64_t>(out), n);   // [memory] heap_lazy_commit: ReadFile fails on uncommitted pages
        const mem::ww::OsWriteScope ws(out, n);   // ReadFile fails on a page the write tracker made read-only
        while (n > 0) {
            const DWORD chunk = static_cast<DWORD>(std::min<uint64_t>(n, 1ull << 30));
            OVERLAPPED ov{};
            ov.Offset = static_cast<DWORD>(ofs & 0xFFFFFFFFull);
            ov.OffsetHigh = static_cast<DWORD>(ofs >> 32);
            DWORD got = 0;
            // A short read means the file shrank since build(): an error, not zeros.
            if (!ReadFile(box->h, out, chunk, &got, &ov) || got != chunk) return false;
            out += chunk;
            ofs += chunk;
            n -= chunk;
        }
        return true;
    }
};

void romfs_prescan(const std::vector<std::string>& folders) {
    PrescanState& ps = prescan_state();
    if (ps.started.exchange(true)) return;
    std::vector<std::wstring> hosts;
    {
        std::lock_guard<std::mutex> lk(ps.mu);
        for (const std::string& f : folders) {
            std::wstring w;
            std::error_code ec;
            if (f.empty() || nup::is_nup(f) || !to_wide(f, w)) continue;
            const std::filesystem::path abs = std::filesystem::absolute(std::filesystem::path(w), ec);
            if (ec || !std::filesystem::is_directory(abs, ec)) continue;
            const std::wstring host = long_path(abs.wstring());
            if (ps.trees.count(host)) continue;
            ps.trees[host];   // pending: a build() that needs it waits
            hosts.push_back(host);
        }
    }
    if (hosts.empty()) return;
    NXLOG_I("fs", "romfs: reading %zu folder(s) ahead on a background thread ([fs] prescan)", hosts.size());
    std::thread([hosts] {
        PrescanState& ps = prescan_state();
        for (const std::wstring& h : hosts) {
            Node n;
            n.is_dir = true;
            n.host = h;
            std::string msg;
            const auto t0 = std::chrono::steady_clock::now();
            const bool ok = scan_tree(n, msg);
            const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            {
                std::lock_guard<std::mutex> lk(ps.mu);
                auto it = ps.trees.find(h);
                if (it != ps.trees.end()) {
                    it->second.node = std::move(n);
                    it->second.ok = ok;
                    it->second.msg = msg;
                    it->second.seconds = sec;
                    it->second.done = true;
                }
            }
            ps.cv.notify_all();
        }
    }).detach();
}

uint32_t romfs_prescan_taken() { return prescan_state().taken.load(std::memory_order_relaxed); }

RomfsImage::RomfsImage() : p_(std::make_unique<Impl>()) {}
RomfsImage::~RomfsImage() = default;

bool RomfsImage::build(const std::string& host_dir, std::string* err, const std::vector<std::string>& overlays, const RomfsEdits* edits) {
    Layout fresh;
    const bool ok = make_layout(host_dir, overlays, edits, fresh, err);
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->cache.clear();
    p_->l = ok ? std::move(fresh) : Layout{};
    p_->read_seen.assign(p_->l.files.size(), 0);
    return ok;
}

bool RomfsImage::built() const { return p_->l.built; }
uint64_t RomfsImage::size() const { return p_->l.size; }
RomfsImage::Stats RomfsImage::stats() const { return p_->l.stats; }

bool RomfsImage::read(uint64_t offset, void* dst, uint64_t len) const {
    const Layout& L = p_->l;
    if (!L.built || offset > L.size || len > L.size - offset) return false;
    if (len == 0) return true;
    uint8_t* out = static_cast<uint8_t*>(dst);
    auto it = std::upper_bound(L.segs.begin(), L.segs.end(), offset,
                               [](uint64_t v, const Segment& g) { return v < g.off; });
    size_t i = static_cast<size_t>(it - L.segs.begin()) - 1;
    while (len > 0) {
        const Segment& g = L.segs[i];
        const uint64_t within = offset - g.off;
        const uint64_t n = std::min(len, g.len - within);
        switch (g.kind) {
            case Segment::Kind::kHeader: std::memcpy(out, L.header.data() + g.src + within, n); break;
            case Segment::Kind::kTables: std::memcpy(out, L.tables.data() + g.src + within, n); break;
            case Segment::Kind::kZero: std::memset(out, 0, n); break;
            case Segment::Kind::kFile:
                p_->note_read(g.file);
                if (!p_->read_host(g.file, g.src + within, out, n)) return false;
                break;
            case Segment::Kind::kMem:
                p_->note_read(g.file);
                std::memcpy(out, L.files[g.file].mem->data() + within, n);
                break;
        }
        out += n;
        offset += n;
        len -= n;
        i++;
    }
    return true;
}

bool RomfsImage::file_extent(const std::string& path, uint64_t* offset, uint64_t* size) const {
    const Layout& L = p_->l;
    auto it = L.by_path.find(path);
    if (it == L.by_path.end()) return false;
    const FileRec& f = L.files[it->second];
    if (offset) *offset = f.image_off;
    if (size) *size = f.size;
    return true;
}

}  // namespace nxrt::services::fs
