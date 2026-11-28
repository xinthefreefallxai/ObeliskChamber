// SPDX-License-Identifier: GPL-3.0-or-later
// Pipeline prewarm (S10). A graphics pipeline is created the first time a draw needs its combination of shaders and baked state,
// and on the real GPU that costs from a few to a few hundred milliseconds on the render thread. Two things keep it off the frame:
//
//  * Recording. Every key a draw asks for is appended to the key file ([nvn] pipeline_keys, default pipeline_keys.bin), whether it was built,
//    linked or had to wait for its parts: the hashes of its shaders (the cache's code hashes, stable between runs) and the rest of the key
//    (formats, blend, topology ...) as plain bytes. The file is in the order the keys were first used, so the ones boot needs come first.
//  * Prewarming. At start every recorded key is queued in that order. Worker threads build each one whole with the on-disk VkPipelineCache,
//    from stand-in shaders of the shader cache (ShaderCache::load_stub: the same SPIR-V and reflection the game's own ShaderInfo gets, and the
//    same shared pipeline layout), so nothing waits for the game to set a program's shaders. The four shader parts of each key are also queued
//    for the pipeline-library threads (nvn_gpl.cpp, gpl_prebuild). When the game sets a program's shaders, the keys that use them go first.
//    A draw finds its pipeline by content (shader hashes and key bytes): it takes the finished one, links one from the parts while the whole
//    one is still being built (the whole one then replaces it), or builds it itself.
//
// [nvn] prewarm_budget (0.3.2, S3): NVIDIA's driver keeps about 1 MB of RAM for every pipeline built whole, and a player's key file only grows
// (2,000+ keys after a few chapters; the 2026-11-22 census put the prewarm at ~1.2 GB of driver memory). So at boot only the `budget` keys a
// draw used most recently are queued (a side file, <keys>.seen, keeps the session each key was last used in); the others wait until the game
// sets one of their programs, as with prewarm_boot = false. 0 or "all": every key at boot (before 0.3.2).
//
// [nvn] cache_rewarm (0.3.2): the keys the budget leaves out are built into the pipeline cache anyway, one at a time on a background thread at
// the lowest priority once the boot queue is done, and each pipeline is destroyed as soon as it is made: the cache keeps the compiled code,
// the driver no longer holds the pipeline itself (on NVIDIA the RAM of its arena stays committed and is reused for later pipelines). A key the game later asks for then builds from the cache in a millisecond or two instead
// of compiling. A side file, <keys>.rewarm, lists the keys known to be in the cache file under this GPU and driver build (the cache's
// identity); after a driver update the identity changes, the list starts over and the work resumes across sessions until it is done.
//
// The VkPipelineCache is also written to disk here: a background thread saves it while new pipelines appear, because a window that is
// closed ends the process without running the shutdown path. The same thread logs a summary line every 30 s.
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/cpu_limit.h"
#include "nxrt/ftrace.h"
#include "nxrt/gauge.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "nvn_gpu_internal.h"
#include "nvn_pipeline.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace nxrt::nvn::be::gpu {
namespace {

constexpr const char* kTag = "nvn.prewarm";
constexpr char kMagic[8] = {'N', 'X', 'P', 'K', 'E', 'Y', 'S', '1'};
constexpr size_t kStageBytes = sizeof(const ShaderInfo*) * 3;
constexpr size_t kRestBytes = offsetof(GraphicsKey, tess) - kStageBytes;   // the tessellation stages are not recorded (tessellated keys never are)
static_assert(offsetof(GraphicsKey, stage) == 0, "the shader pointers come first in GraphicsKey");
static_assert(offsetof(GraphicsKey, color_format) == kStageBytes, "the plain key bytes follow the shader pointers");
constexpr size_t kMaxRecords = 400000;

struct Rec {
    uint64_t h[3];                          // code hashes of vertex, fragment, geometry (0 = stage absent)
    uint8_t rest[kRestBytes];               // the key after the shader pointers, byte for byte
};

// One recorded key being built ahead. Guarded by g_mu.
struct Job {
    Rec rec;
    uint64_t digest = 0;
    int state = 0;                          // 0 queued, 1 building, 2 done, 3 taken over by a draw (the worker skips it)
    bool in_queue = false, promoted = false;
    VkPipeline pipe = VK_NULL_HANDLE;
    std::shared_ptr<PipeUpgrade> up;        // a draw linked the key from parts meanwhile: the whole pipeline goes here and replaces that one
};

std::mutex g_mu;
std::condition_variable g_cv;
bool g_inited = false, g_enabled = true, g_stop = false;
std::string g_path;
std::vector<Rec> g_recs;
std::vector<std::shared_ptr<Job>> g_rec_jobs;                // by index in g_recs (null: recorded this session, built at its draw)
// Programs of one stage each (Pokopia binds a vertex program and a fragment program separately): a record is found by any of its stages.
std::unordered_multimap<uint64_t, size_t> g_by_any;          // any stage's hash -> index in g_recs
std::unordered_set<uint64_t> g_digests;                      // recorded keys
std::deque<std::shared_ptr<Job>> g_queue;                    // a job can sit in it twice (moved ahead): the second visit skips it
std::unordered_map<uint64_t, std::vector<std::shared_ptr<Job>>> g_jobs;   // by digest; a job leaves when a draw takes it or takes it over
std::vector<std::thread> g_workers;
std::thread g_saver;
PrewarmStats g_stats;
uint64_t g_promote_skipped = 0;   // keys prewarm_promote_sessions left for their draw; guarded by g_mu

// <keys>.seen: the session each key was last used in by a draw (prewarm_budget orders the boot queue by it). Guarded by g_mu.
constexpr char kSeenMagic[8] = {'N', 'X', 'P', 'K', 'S', 'E', 'E', 'N'};
std::unordered_map<uint64_t, uint32_t> g_seen;               // digest -> session
uint32_t g_session = 1;                                       // this session's number (the file's + 1)
bool g_seen_dirty = false;
size_t g_budget = 0;                                          // keys queued at boot (0: all)
size_t g_deferred = 0;                                        // recorded keys left out of the boot queue by the budget

std::string seen_path() { return g_path + ".seen"; }

// [nvn] cache_rewarm. Guarded by g_mu.
constexpr char kRewarmMagic[8] = {'N', 'X', 'P', 'K', 'R', 'W', 'R', 'M'};
std::thread g_rewarm;
bool g_rewarm_on = false;
std::string g_rw_identity;                                    // the cache identity the done list belongs to
std::unordered_set<uint64_t> g_rw_done;                       // keys in the cache file as last saved
std::vector<uint64_t> g_rw_pending;                           // keys built into the cache since the last save of the file
bool g_rw_dirty = false;
struct RewarmStats { uint64_t todo = 0, built = 0, failed = 0, skipped = 0; double ms = 0; std::vector<float> each; bool finished = false; } g_rw;
// Build times of the first boot prewarm builds (FR4). RTX 3070 Laptop, 2026-11-22: 0.7-0.9 ms each from a warm cache, 8.7-10.3 ms cold.
std::vector<float> g_boot_ms;
bool g_boot_ms_logged = false;

std::string rewarm_path() { return g_path + ".rewarm"; }

void load_rewarm_locked(const std::string& identity) {
    g_rw_done.clear();
    std::ifstream f(rewarm_path(), std::ios::binary);
    if (!f) return;
    char magic[8], id[64] = {};
    uint32_t ver = 0;
    f.read(magic, 8);
    f.read(reinterpret_cast<char*>(&ver), 4);
    f.read(id, sizeof id);
    if (!f || std::memcmp(magic, kRewarmMagic, 8) != 0 || ver != 1) return;
    if (identity != std::string(id, strnlen(id, sizeof id))) {
        NXLOG_I(kTag, "cache rewarm: the GPU or driver build changed since the last list (%s, now %s): it starts over", std::string(id, strnlen(id, sizeof id)).c_str(),
                identity.c_str());
        return;
    }
    uint64_t d;
    while (f.read(reinterpret_cast<char*>(&d), 8)) g_rw_done.insert(d);
}

void save_rewarm_locked() {
    const std::string tmp = rewarm_path() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return;
        const uint32_t ver = 1;
        char id[64] = {};
        std::memcpy(id, g_rw_identity.data(), std::min(g_rw_identity.size(), sizeof id - 1));
        f.write(kRewarmMagic, 8);
        f.write(reinterpret_cast<const char*>(&ver), 4);
        f.write(id, sizeof id);
        for (uint64_t d : g_rw_done) f.write(reinterpret_cast<const char*>(&d), 8);
        if (!f) return;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, rewarm_path(), ec);
    if (ec) {
        std::filesystem::remove(rewarm_path(), ec);
        std::filesystem::rename(tmp, rewarm_path(), ec);
    }
    g_rw_dirty = false;
}

// The cache file was just written: what was built into the cache until then is in it.
void rewarm_note_saved_locked() {
    // before the rewarm thread has read the list and set the identity, the list is not written: it would go out with no identity and be
    // thrown away as "another driver" when the thread reads it (the pending keys wait for the next save)
    if (!g_rewarm_on || g_rw_pending.empty() || g_rw_identity.empty()) return;
    for (uint64_t d : g_rw_pending) g_rw_done.insert(d);
    g_rw_pending.clear();
    save_rewarm_locked();
}

float median_of(std::vector<float> v) {
    if (v.empty()) return 0.0f;
    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
    return v[v.size() / 2];
}

// [nvn] cache_rewarm: auto (default) = on when prewarm_budget left keys out of the boot queue; true; false.
bool rewarm_from_config(size_t deferred) {
    std::string v = config_get_str("nvn", "cache_rewarm", "auto");
    for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (v == "auto" || v.empty()) return deferred > 0;
    return !(v == "false" || v == "0" || v == "off" || v == "no");
}

void load_seen() {
    std::ifstream f(seen_path(), std::ios::binary);
    if (!f) return;
    char magic[8];
    uint32_t ver = 0, session = 0;
    f.read(magic, 8);
    f.read(reinterpret_cast<char*>(&ver), 4);
    f.read(reinterpret_cast<char*>(&session), 4);
    if (!f || std::memcmp(magic, kSeenMagic, 8) != 0 || ver != 1) return;
    g_session = session + 1;
    struct E { uint64_t d; uint32_t s, pad; } e;
    while (f.read(reinterpret_cast<char*>(&e), sizeof e)) g_seen[e.d] = e.s;
}

// Written whole (small: 16 bytes a key) by the saver thread when it changed, and once at the start so the session number moves on.
void save_seen_locked() {
    const std::string tmp = seen_path() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return;
        const uint32_t ver = 1;
        f.write(kSeenMagic, 8);
        f.write(reinterpret_cast<const char*>(&ver), 4);
        f.write(reinterpret_cast<const char*>(&g_session), 4);
        struct E { uint64_t d; uint32_t s, pad; };
        for (const auto& [d, sess] : g_seen) {
            const E e{d, sess, 0};
            f.write(reinterpret_cast<const char*>(&e), sizeof e);
        }
        if (!f) return;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, seen_path(), ec);
    if (ec) {
        std::filesystem::remove(seen_path(), ec);
        std::filesystem::rename(tmp, seen_path(), ec);
    }
    g_seen_dirty = false;
}

void mark_seen_locked(uint64_t digest) {
    uint32_t& s = g_seen[digest];
    if (s != g_session) {
        s = g_session;
        g_seen_dirty = true;
    }
}

// [nvn] prewarm_budget: auto (default) = every key on PCs over 20 GiB of RAM with the limits not scaled, else 800 at 16 GB and 400 at 8 GB
// ([memory] scale_limits); a number; 0 or "all" = every key.
size_t budget_from_config() {
    std::string v = config_get_str("nvn", "prewarm_budget", "auto");
    for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (v == "all") return 0;
    if (v == "auto" || v.empty()) return static_cast<size_t>(std::max<int64_t>(0, nxrt::mem::limit_by_ram(0, 800, 400)));
    char* end = nullptr;
    const long long n = std::strtoll(v.c_str(), &end, 10);
    if (end == v.c_str() || n < 0) {
        NXLOG_W(kTag, "[nvn] prewarm_budget = %s is not auto, all or a number: auto is used", v.c_str());
        return static_cast<size_t>(std::max<int64_t>(0, nxrt::mem::limit_by_ram(0, 800, 400)));
    }
    return static_cast<size_t>(n);
}

uint64_t parse_hash(const std::string& hex) {
    uint64_t v = 0;
    for (char ch : hex) {
        uint64_t d;
        if (ch >= '0' && ch <= '9') d = static_cast<uint64_t>(ch - '0');
        else if (ch >= 'a' && ch <= 'f') d = static_cast<uint64_t>(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F') d = static_cast<uint64_t>(ch - 'A' + 10);
        else return 0;
        v = (v << 4) | d;
    }
    return v;
}

std::string hex16(uint64_t h) {
    char b[20];
    std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(h));
    return b;
}

uint64_t digest_of(const uint64_t h[3], const uint8_t* rest) {
    uint64_t d = 1469598103934665603ull;
    auto mix = [&d](uint8_t b) { d = (d ^ b) * 1099511628211ull; };
    for (int i = 0; i < 3; ++i)
        for (int k = 0; k < 8; ++k) mix(static_cast<uint8_t>(h[i] >> (8 * k)));
    for (size_t i = 0; i < kRestBytes; ++i) mix(rest[i]);
    return d;
}

// The record of a key; false when it cannot be recorded (tessellation, a stage the cache has no hash for).
bool rec_of(const GraphicsKey& key, Rec* r) {
    if (key.tess[0] || key.tess[1]) return false;
    std::memset(r, 0, sizeof *r);
    for (int i = 0; i < 3; ++i) r->h[i] = key.stage[i] ? parse_hash(key.stage[i]->hash) : 0;
    if (!r->h[0]) return false;
    std::memcpy(r->rest, reinterpret_cast<const uint8_t*>(&key) + kStageBytes, kRestBytes);
    return true;
}

// The key of a record with stand-in shaders from the shader cache, and its layout (the shared one of the same descriptors, so the same
// VkPipelineLayout the game's program gets). Any thread; takes no lock of this file.
// G4: a record made while [nvn] feedback_loop_layout was in use (GraphicsKey::fb_loop) on a device without the layout path: no draw of
// this session asks for that key, so it is not built (the file keeps it for a session with the switch on).
bool rec_unwanted(const Rec& r) {
    constexpr size_t kFbLoopAt = offsetof(GraphicsKey, fb_loop) - kStageBytes;
    static_assert(kFbLoopAt < kRestBytes, "fb_loop is among the recorded key bytes");
    return r.rest[kFbLoopAt] != 0 && !fb_loop_usage_on();
}

bool stub_key(const Rec& r, GraphicsKey& k, const ProgLayout*& pl) {
    if (!shader_cache().is_open() || rec_unwanted(r)) return false;
    std::memset(&k, 0, sizeof k);
    for (int i = 0; i < 3; ++i) {
        if (!r.h[i]) continue;
        k.stage[i] = shader_cache().load_stub(hex16(r.h[i]));
        if (!k.stage[i]) return false;
    }
    std::memcpy(reinterpret_cast<uint8_t*>(&k) + kStageBytes, r.rest, kRestBytes);
    const ShaderInfo* st6[6] = {k.stage[0], k.stage[1], k.stage[2], nullptr, nullptr, nullptr};
    pl = prog_layout(st6);
    return pl != nullptr;
}

// The parts of a record, for the pipeline-library threads.
void queue_parts(const Rec& r, bool front) {
    gpl_prebuild([r](GraphicsKey& k, const ProgLayout*& pl) { return stub_key(r, k, pl); }, front);
}

void erase_job_locked(const std::shared_ptr<Job>& j) {
    auto it = g_jobs.find(j->digest);
    if (it == g_jobs.end()) return;
    auto& vec = it->second;
    vec.erase(std::remove(vec.begin(), vec.end(), j), vec.end());
    if (vec.empty()) g_jobs.erase(it);
}

void load_file() {
    std::ifstream f(g_path, std::ios::binary);
    if (!f) return;
    char magic[8];
    uint32_t size = 0, pad = 0;
    f.read(magic, 8);
    f.read(reinterpret_cast<char*>(&size), 4);
    f.read(reinterpret_cast<char*>(&pad), 4);
    if (!f || std::memcmp(magic, kMagic, 8) != 0 || size != kRestBytes) {
        NXLOG_W(kTag, "%s is not a key file of this build (the record layout changed): ignored, a new one is started", g_path.c_str());
        f.close();
        std::error_code ec;
        std::filesystem::remove(g_path, ec);                 // so the next record writes a fresh header
        return;
    }
    Rec r;
    while (g_recs.size() < kMaxRecords && f.read(reinterpret_cast<char*>(&r), sizeof r)) {
        if (!g_digests.insert(digest_of(r.h, r.rest)).second) continue;
        for (uint64_t h : r.h)
            if (h) g_by_any.emplace(h, g_recs.size());
        g_recs.push_back(r);
    }
    g_stats.loaded = g_recs.size();
}

void append_record(const Rec& r) {
    bool fresh = false;
    {
        std::ifstream t(g_path, std::ios::binary | std::ios::ate);
        fresh = !t || t.tellg() < 16;
    }
    std::ofstream f(g_path, std::ios::binary | std::ios::app);
    if (!f) return;
    if (fresh) {
        const uint32_t size = static_cast<uint32_t>(kRestBytes), pad = 0;
        f.write(kMagic, 8);
        f.write(reinterpret_cast<const char*>(&size), 4);
        f.write(reinterpret_cast<const char*>(&pad), 4);
    }
    f.write(reinterpret_cast<const char*>(&r), sizeof r);
}

void worker_main() {
    log_set_thread_name("nvn-prewarm");
    set_compile_thread_priority();
    for (;;) {
        std::shared_ptr<Job> j;
        bool urgent = false;                   // moved ahead: the game set one of its programs, a draw is near
        {
            std::unique_lock<std::mutex> lk(g_mu);
            g_cv.wait(lk, [] { return g_stop || !g_queue.empty(); });
            if (g_stop) return;
            j = std::move(g_queue.front());
            g_queue.pop_front();
            if (j->state != 0) continue;       // built already (it was moved ahead) or a draw took it over
            j->state = 1;
            urgent = j->promoted;
        }
        VkPipeline p = VK_NULL_HANDLE;
        const auto t0 = std::chrono::steady_clock::now();
        {
            NXFT("prewarm.build");
            GraphicsKey k;
            const ProgLayout* pl = nullptr;
            if (stub_key(j->rec, k, pl)) p = build_graphics_pipeline(k, *pl);
        }
        const float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
        bool next_urgent = false;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            j->state = 2;
            if (p && g_rewarm_on) g_rw_pending.push_back(j->digest);   // in the cache now, whoever built it
            if (p && g_boot_ms.size() < 64) {
                g_boot_ms.push_back(ms);
                if (g_boot_ms.size() == 64 && !g_boot_ms_logged) {
                    g_boot_ms_logged = true;
                    const float med = median_of(g_boot_ms);
                    NXLOG_I(kTag, "prewarm: the first 64 builds took %.1f ms each (median): the driver's cache %s", med,
                            med < 2.5f ? "has them (warm)" : med < 5.0f ? "has some of them" : "is cold (a new driver, a new GPU or a cleared cache)");
                }
            }
            for (const auto& n : g_queue) {    // the next job this thread would take: no sleep before one a draw is near
                if (n->state != 0) continue;
                next_urgent = n->promoted;
                break;
            }
            if (p) ++g_stats.built; else ++g_stats.failed;
            if (j->up) {                       // a draw linked it meanwhile: this one replaces the linked one (gpl_take_upgrade)
                j->up->pipe = p;
                j->up->state.store(1, std::memory_order_release);
                j->up.reset();
            } else {
                j->pipe = p;
            }
        }
        g_cv.notify_all();
        if (!urgent && !next_urgent) pace_shader_work();   // a key-file pipeline built ahead: nothing waits for it yet
    }
}

// Writes the cache file now and lists what went into it (from the rewarm thread, which holds no lock here).
void save_rewarm_progress() {
    size_t pending;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        pending = g_rw_pending.size();
    }
    if (!pending || !pipeline_cache_save()) return;
    std::lock_guard<std::mutex> lk(g_mu);
    std::vector<uint64_t> rest(g_rw_pending.begin() + static_cast<std::ptrdiff_t>(std::min(pending, g_rw_pending.size())), g_rw_pending.end());
    g_rw_pending.resize(std::min(pending, g_rw_pending.size()));
    rewarm_note_saved_locked();
    g_rw_pending = std::move(rest);
}

// [nvn] cache_rewarm: once the boot queue is done, the keys left out of it go into the pipeline cache one by one (see the top of the file).
void rewarm_main() {
    log_set_thread_name("nvn-rewarm");
    set_compile_thread_priority();
#if defined(_WIN32)
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_IDLE);   // below the other compile threads: only spare time
#endif
    {
        std::unique_lock<std::mutex> lk(g_mu);
        // the boot queue first, then a quiet minute: loads and the first visit to a place promote keys of their own
        for (;;) {
            g_cv.wait_for(lk, std::chrono::seconds(2), [] { return g_stop; });
            if (g_stop) return;
            bool busy = !g_queue.empty();
            for (const auto& j : g_rec_jobs)
                if (j && j->state == 1) { busy = true; break; }
            if (!busy) break;
        }
        const auto settle = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        g_cv.wait_until(lk, settle, [] { return g_stop; });
        if (g_stop) return;
    }
    const PipelineCacheOrigin origin = pipeline_cache_open();
    const std::string identity = pipeline_cache_identity();
    std::vector<std::shared_ptr<Job>> todo;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_rw_identity = identity;
        load_rewarm_locked(identity);
        if (origin != PipelineCacheOrigin::loaded && !g_rw_done.empty()) {   // the file was not used: nothing listed is in the cache now
            NXLOG_I(kTag, "cache rewarm: the pipeline cache file was not used this time: the list of %zu keys starts over", g_rw_done.size());
            g_rw_done.clear();
            g_rw_dirty = true;
        }
        // most recently used first, as the boot queue
        auto last = [](const std::shared_ptr<Job>& j) {
            auto it = g_seen.find(j->digest);
            return it == g_seen.end() ? 0u : it->second;
        };
        for (const auto& j : g_rec_jobs) {
            if (!j || j->in_queue || j->state != 0) continue;   // queued or built this session: in the cache already
            if (g_rw_done.count(j->digest)) { ++g_rw.skipped; continue; }
            todo.push_back(j);
        }
        std::stable_sort(todo.begin(), todo.end(), [&](const auto& a, const auto& b) { return last(a) > last(b); });
        g_rw.todo = todo.size();
        NXLOG_I(kTag, "cache rewarm: %zu keys left out of the boot queue go into the pipeline cache in the background (%llu already there from earlier sessions; "
                      "cache file %s; [nvn] cache_rewarm)",
                todo.size(), static_cast<unsigned long long>(g_rw.skipped),
                origin == PipelineCacheOrigin::loaded ? "loaded" : origin == PipelineCacheOrigin::stale ? "out of date" : origin == PipelineCacheOrigin::none ? "new" : "unreadable");
    }
    const auto t_start = std::chrono::steady_clock::now();
    for (const auto& j : todo) {
        {
            std::lock_guard<std::mutex> lk(g_mu);
            if (g_stop) return;
            if (j->in_queue || j->state != 0) continue;            // the game set its program meanwhile: a prewarm worker has it
        }
        GraphicsKey k;
        const ProgLayout* pl = nullptr;
        VkPipeline p = VK_NULL_HANDLE;
        const auto t0 = std::chrono::steady_clock::now();
        if (stub_key(j->rec, k, pl)) {
            NXFT("prewarm.rewarm");
            p = build_graphics_pipeline(k, *pl);
        }
        const float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (p) vkDestroyPipeline(vkdev(), p, nullptr);              // the cache keeps the code; the pipeline itself is not wanted yet
        {
            std::lock_guard<std::mutex> lk(g_mu);
            if (p) {
                ++g_rw.built;
                g_rw.ms += ms;
                if (g_rw.each.size() < 4096) g_rw.each.push_back(ms);
                g_rw_pending.push_back(j->digest);
            } else {
                ++g_rw.failed;
            }
        }
        pace_shader_work();
        if (g_rw.built && g_rw.built % 1000 == 0) save_rewarm_progress();   // a big cache file is otherwise rewritten only after 3,000 new pipelines
    }
    save_rewarm_progress();
    std::lock_guard<std::mutex> lk(g_mu);
    g_rw.finished = true;
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
    const float med = median_of(g_rw.each);
    NXLOG_I(kTag, "cache rewarm: done, %llu keys built into the pipeline cache (%llu failed) in %.1f s, %.1f ms of building each (median, %s)",
            static_cast<unsigned long long>(g_rw.built), static_cast<unsigned long long>(g_rw.failed), s, med,
            med < 2.5f ? "they were in the driver's cache" : "compiled from scratch");
}

// The summary line every 30 s: what was deferred, built at the draw and built in the background since the last one (session totals in brackets).
void log_summary_locked() {
    struct Snap { uint64_t deferred = 0, linked = 0, whole_draw = 0, whole_bg = 0, parts_bg = 0, upgrades = 0; };
    static Snap last;
    const GplStats g = gpl_stats();
    Snap now;
    now.deferred = g.draws_deferred;
    now.linked = g.linked;
    now.whole_draw = pipelines_built_at_draw();
    now.whole_bg = g_stats.built;
    now.parts_bg = g.parts_ahead;
    now.upgrades = g.upgrades_done;
    size_t waiting = 0;
    for (const auto& j : g_rec_jobs)
        if (j && j->state <= 1) ++waiting;
    auto d = [](uint64_t a, uint64_t b) { return static_cast<unsigned long long>(a - b); };
    auto u = [](uint64_t a) { return static_cast<unsigned long long>(a); };
    NXLOG_I("nvn.pipe", "pipelines, last 30 s (session): draws deferred %llu (%llu), linked at the draw %llu (%llu), built whole at the draw %llu (%llu); "
                        "background builds %llu (%llu): whole %llu, parts %llu, upgrades %llu; keys known %zu (%llu from the file, %llu new), %zu still to build",
            d(now.deferred, last.deferred), u(now.deferred), d(now.linked, last.linked), u(now.linked), d(now.whole_draw, last.whole_draw), u(now.whole_draw),
            d(now.whole_bg + now.parts_bg + now.upgrades, last.whole_bg + last.parts_bg + last.upgrades), u(now.whole_bg + now.parts_bg + now.upgrades),
            d(now.whole_bg, last.whole_bg), d(now.parts_bg, last.parts_bg), d(now.upgrades, last.upgrades), g_recs.size(), u(g_stats.loaded), u(g_stats.recorded),
            waiting);
    last = now;
}

void saver_main() {
    log_set_thread_name("nvn-cache-save");
    std::unique_lock<std::mutex> lk(g_mu);
    uint64_t logged = 0;
    auto next_summary = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!g_stop) {
        g_cv.wait_for(lk, std::chrono::seconds(2), [] { return g_stop; });
        if (g_stop) break;
        const uint64_t sig = g_stats.recorded + g_stats.queued * 3 + g_stats.built * 5 + g_stats.taken * 7 + g_stats.waited * 11 + g_stats.claimed * 13 +
                             g_stats.linked_meanwhile * 17 + g_stats.promoted * 19 + g_stats.skipped_busy * 23 + g_promote_skipped * 29;
        if (sig != logged) {                       // the process may end without a shutdown: say what the prewarm did while it runs
            logged = sig;
            NXLOG_I(kTag, "prewarm so far: %llu recorded, %llu loaded, %llu queued (%llu moved ahead), %llu built ahead (%llu failed), %llu taken by draws (%llu waited %.1f ms), "
                          "%llu linked from parts while building, %llu built by the draw itself, %llu draws skipped while their build ran, "
                          "%llu not moved ahead (not drawn lately)",
                    static_cast<unsigned long long>(g_stats.recorded), static_cast<unsigned long long>(g_stats.loaded), static_cast<unsigned long long>(g_stats.queued),
                    static_cast<unsigned long long>(g_stats.promoted), static_cast<unsigned long long>(g_stats.built), static_cast<unsigned long long>(g_stats.failed),
                    static_cast<unsigned long long>(g_stats.taken), static_cast<unsigned long long>(g_stats.waited), g_stats.wait_ms,
                    static_cast<unsigned long long>(g_stats.linked_meanwhile), static_cast<unsigned long long>(g_stats.claimed),
                    static_cast<unsigned long long>(g_stats.skipped_busy), static_cast<unsigned long long>(g_promote_skipped));
        }
        gpl_progress_log();
        compute_progress_log();
        if (std::chrono::steady_clock::now() >= next_summary) {
            next_summary += std::chrono::seconds(30);
            log_summary_locked();
            if (g_seen_dirty) save_seen_locked();
        }
        if (pipeline_cache_dirty()) {
            const size_t pending = g_rw_pending.size();
            lk.unlock();
            const bool saved = pipeline_cache_save();
            lk.lock();
            if (saved && pending) {
                // only what was pending before the write is surely in the file
                std::vector<uint64_t> rest(g_rw_pending.begin() + static_cast<std::ptrdiff_t>(pending), g_rw_pending.end());
                g_rw_pending.resize(pending);
                rewarm_note_saved_locked();
                g_rw_pending = std::move(rest);
            }
        } else if (g_rw_dirty && !g_rw_identity.empty()) {
            save_rewarm_locked();
        }
    }
}

}  // namespace

void prewarm_init() {
    std::vector<Rec> parts;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_inited) return;
        g_inited = true;
        g_stop = false;
        g_enabled = config_get_bool("nvn", "prewarm", true);
        g_path = config_get_str("nvn", "pipeline_keys", "pipeline_keys.bin");
        if (!g_enabled || g_path.empty()) {
            g_enabled = false;
            return;
        }
        load_file();
        load_seen();
        // [nvn] prewarm_boot = false: a key waits until the game sets one of its shaders (the old behaviour)
        const bool boot = config_get_bool("nvn", "prewarm_boot", true);
        g_budget = budget_from_config();
        g_rec_jobs.resize(g_recs.size());
        for (size_t i = 0; i < g_recs.size(); ++i) {
            auto j = std::make_shared<Job>();
            j->rec = g_recs[i];
            j->digest = digest_of(j->rec.h, j->rec.rest);
            if (rec_unwanted(j->rec)) j->state = 2;              // never queued, promoted or rewarmed (G4)
            g_rec_jobs[i] = j;
            g_jobs[j->digest].push_back(j);
        }
        // The boot queue: most recently used first (the session a draw last used the key in; keys from before the .seen file count as
        // session 0), then in the order they were first used. Only the first `budget` go in; the rest when the game sets their programs.
        std::vector<size_t> order;
        order.reserve(g_recs.size());
        for (size_t i = 0; i < g_recs.size(); ++i)
            if (g_rec_jobs[i]->state == 0) order.push_back(i);
        auto last = [](const std::shared_ptr<Job>& j) {
            auto it = g_seen.find(j->digest);
            return it == g_seen.end() ? 0u : it->second;
        };
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return last(g_rec_jobs[a]) > last(g_rec_jobs[b]); });
        size_t newest = 0, never = 0;
        for (size_t i : order) {
            const uint32_t s = last(g_rec_jobs[i]);
            if (s + 1 == g_session) ++newest;
            if (!s) ++never;
        }
        const size_t n = !boot ? 0 : (g_budget ? std::min(g_budget, order.size()) : order.size());
        for (size_t k = 0; k < n; ++k) {
            const auto& j = g_rec_jobs[order[k]];
            j->in_queue = true;
            g_queue.push_back(j);
            parts.push_back(j->rec);
        }
        g_deferred = boot ? order.size() - n : 0;
        g_stats.queued = n;
        save_seen_locked();                                     // the session number moves on even if nothing is drawn
        const unsigned hw = std::max(1u, nxrt::host_threads());
        const int64_t want = config_get_int("nvn", "prewarm_threads", std::clamp<int64_t>(hw / 4, 1, 6));   // was hw / 6, 1 to 4 (2 on 16 threads)
        for (int64_t i = 0; i < want; ++i) g_workers.emplace_back(worker_main);
        g_saver = std::thread(saver_main);
        g_rewarm_on = boot && rewarm_from_config(g_deferred);
        if (g_rewarm_on) g_rewarm = std::thread(rewarm_main);
        if (!boot)
            NXLOG_I(kTag, "prewarm: %zu recorded pipelines from %s, %zu worker threads; each queued when the game sets one of its shaders ([nvn] prewarm_boot is false)",
                    g_recs.size(), g_path.c_str(), g_workers.size());
        else if (!g_deferred)
            NXLOG_I(kTag, "prewarm: %zu recorded pipelines from %s, %zu worker threads; all queued from boot, most recently used first ([nvn] prewarm_budget %s; "
                          "session %u: %zu used last session, %zu with no use on record)",
                    g_recs.size(), g_path.c_str(), g_workers.size(), g_budget ? "not reached" : "= all", g_session, newest, never);
        else
            NXLOG_I(kTag, "prewarm: %zu recorded pipelines from %s, %zu worker threads; the %zu most recently used queued from boot ([nvn] prewarm_budget), %zu wait "
                          "until the game sets their programs (session %u: %zu used last session, %zu with no use on record)",
                    g_recs.size(), g_path.c_str(), g_workers.size(), n, g_deferred, g_session, newest, never);
    }
    for (const Rec& r : parts) queue_parts(r, false);           // outside g_mu: the pipeline-library side takes its own lock
}

void prewarm_record(const GraphicsKey& key) {
    Rec r;
    if (!rec_of(key, &r)) return;                               // the record has no room for tessellation stages
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_enabled) return;
    if (g_recs.size() >= kMaxRecords || !g_digests.insert(digest_of(r.h, r.rest)).second) return;
    g_recs.push_back(r);
    g_rec_jobs.push_back(nullptr);                              // built or linked at its draw this session
    mark_seen_locked(digest_of(r.h, r.rest));
    ++g_stats.recorded;
    append_record(r);
}

// [nvn] prewarm_promote_sessions: when the game sets a program, only the recorded keys a draw used in the last this-many sessions go
// first in line (0 = every key with one of its shaders; auto: 3 with memory limits scaled to the PC, else 0). A shared vertex shader can
// bring hundreds of keys forward: on the RTX 3070 laptop 2040 keys were moved ahead and ~650 drawn, and NVIDIA keeps the RAM of every
// pipeline it built in its own arenas even after the pipeline is destroyed (a 1 GiB arena stayed ~990 MiB in RAM after 2347 releases).
// A key left behind is built or linked from its parts at its first draw, as one the budget left out (warm caches: well under a ms).
uint32_t promote_sessions() {
    static const uint32_t n = [] {
        const std::string v = config_get_str("nvn", "prewarm_promote_sessions", "auto");
        const bool autom = v == "auto" || v.empty();
        const int64_t r = autom ? (mem::limits_scaled() ? 3 : 0) : std::max<int64_t>(0, config_get_int("nvn", "prewarm_promote_sessions", 0));
        NXLOG_I(kTag, "prewarm: when the game sets a program, %s go first ([nvn] prewarm_promote_sessions = %s%s)",
                r ? ("its keys drawn in the last " + std::to_string(r) + " sessions").c_str() : "all its recorded keys", autom ? "auto" : v.c_str(),
                autom ? (mem::limits_scaled() ? ", limits scaled" : ", limits not scaled") : "");
        return static_cast<uint32_t>(r);
    }();
    return n;
}

size_t prewarm_program(const ShaderInfo* const stages[6]) {
    if (!stages || stages[3] || stages[4]) return 0;            // tessellated programs are not recorded
    prewarm_init();
    std::vector<Rec> ahead;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!g_enabled || g_stop) return 0;
        // The game is about to draw with these shaders: every recorded key that has one of them goes first in line.
        for (int i = 0; i < 3; ++i) {
            if (!stages[i]) continue;
            auto range = g_by_any.equal_range(parse_hash(stages[i]->hash));
            for (auto it = range.first; it != range.second; ++it) {
                const std::shared_ptr<Job>& j = g_rec_jobs[it->second];
                if (!j || j->state != 0 || j->promoted) continue;
                if (const uint32_t keep = promote_sessions()) {
                    auto seen = g_seen.find(j->digest);
                    if (seen == g_seen.end() || seen->second + keep < g_session) {   // not drawn lately: its draw builds it
                        if (!j->in_queue) { j->promoted = true; ++g_promote_skipped; }
                        continue;
                    }
                }
                j->promoted = true;
                if (!j->in_queue) {
                    j->in_queue = true;
                    ++g_stats.queued;
                }
                g_queue.push_front(j);
                ++g_stats.promoted;
                ahead.push_back(j->rec);
            }
        }
    }
    if (ahead.empty()) return 0;
    g_cv.notify_all();
    for (const Rec& r : ahead) queue_parts(r, true);
    return ahead.size();
}

bool prewarm_take(const GraphicsKey& key, VkPipeline* out, const std::function<bool()>& can_link, std::shared_ptr<PipeUpgrade>* later, bool* busy) {
    if (busy) *busy = false;
    Rec want;
    if (!rec_of(key, &want)) return false;
    const uint64_t digest = digest_of(want.h, want.rest);
    std::unique_lock<std::mutex> lk(g_mu);
    if (!g_enabled) return false;
    if (g_digests.count(digest)) mark_seen_locked(digest);     // a draw needs this key now: prewarm_budget queues it early next time
    std::shared_ptr<Job> j;
    {
        auto it = g_jobs.find(digest);
        if (it == g_jobs.end()) return false;
        for (const auto& e : it->second)
            if (std::memcmp(&e->rec, &want, sizeof want) == 0) { j = e; break; }
    }
    if (!j) return false;
    bool asked = false;
    for (;;) {
        if (j->state == 2) {
            erase_job_locked(j);
            if (!j->pipe) return false;                          // the build failed: the draw tries itself
            *out = j->pipe;
            j->pipe = VK_NULL_HANDLE;
            ++g_stats.taken;
            return true;
        }
        if (j->state == 3) return false;
        if (!asked && can_link && later) {                       // queued or building: can the draw link it from its parts now?
            asked = true;
            lk.unlock();
            const bool link = can_link();                        // takes the module and pipeline-library locks
            lk.lock();
            if (j->state == 2 || j->state == 3) continue;
            if (link) {
                j->up = std::make_shared<PipeUpgrade>();
                *later = j->up;
                erase_job_locked(j);
                ++g_stats.linked_meanwhile;
                return false;
            }
        }
        if (j->state == 0) {                                     // still queued: build it here instead of behind the others
            j->state = 3;
            erase_job_locked(j);
            ++g_stats.claimed;
            return false;
        }
        if (busy) {                                              // being built and not linkable: the draw is skipped, the next one asks again
            *busy = true;
            ++g_stats.skipped_busy;
            return false;
        }
        const auto t0 = std::chrono::steady_clock::now();        // being built: wait for it
        {
            NXFT("prewarm.wait");
            g_cv.wait(lk, [&] { return j->state == 2 || g_stop; });
        }
        g_stats.wait_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        ++g_stats.waited;
        if (j->state != 2) return false;
    }
}

void prewarm_shutdown() {
    std::vector<std::thread> workers;
    std::thread saver;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!g_inited) return;
        g_stop = true;
        workers.swap(g_workers);
        saver = std::move(g_saver);
        if (g_rewarm.joinable()) workers.push_back(std::move(g_rewarm));
        g_queue.clear();
    }
    g_cv.notify_all();
    for (auto& t : workers) t.join();
    if (saver.joinable()) saver.join();
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_seen_dirty) save_seen_locked();
    for (auto& [h, vec] : g_jobs)
        for (auto& j : vec)
            if (j->state == 2 && j->pipe) vkDestroyPipeline(vkdev(), j->pipe, nullptr);     // built and never used
    g_jobs.clear();
    NXLOG_I(kTag, "prewarm: %llu recorded, %llu loaded, %llu queued, %llu built ahead (%llu failed), %llu taken (%llu waited %.1f ms), %llu linked from parts while building, "
                  "%llu taken over by a draw",
            static_cast<unsigned long long>(g_stats.recorded), static_cast<unsigned long long>(g_stats.loaded), static_cast<unsigned long long>(g_stats.queued),
            static_cast<unsigned long long>(g_stats.built), static_cast<unsigned long long>(g_stats.failed), static_cast<unsigned long long>(g_stats.taken),
            static_cast<unsigned long long>(g_stats.waited), g_stats.wait_ms, static_cast<unsigned long long>(g_stats.linked_meanwhile),
            static_cast<unsigned long long>(g_stats.claimed));
    g_inited = false;
    g_stats = PrewarmStats{};
    g_promote_skipped = 0;
    g_recs.clear();
    g_rec_jobs.clear();
    g_by_any.clear();
    g_digests.clear();
    g_seen.clear();
    g_session = 1;
    g_deferred = 0;
    g_rewarm_on = false;
    g_rw_done.clear();
    g_rw_pending.clear();
    g_rw = RewarmStats{};
    g_boot_ms.clear();
    g_boot_ms_logged = false;
}

PrewarmStats prewarm_stats() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_stats;
}

// The game set a program's shaders: its recorded pipelines go first in line. Programs with both a vertex and a fragment shader also get
// their shader parts queued by the pipeline-library side when the key file has nothing for them.
void prewarm_program_shaders(const ShaderInfo* const stages[6]) {
    if (stages && stages[5]) compute_prebuild(stages[5]);       // a compute program: its pipeline on a background thread ([nvn] compute_async)
    gpl_program(stages, prewarm_program(stages) != 0);
}

void flush_caches() { pipeline_cache_flush(); }

// Gauges (nxrt/gauge.h). The callbacks take g_mu; they are removed before the containers they read are destroyed.
namespace {
struct PrewarmGauges {
    PrewarmGauges() {
        nxrt::gauge_register("nvn.prewarm.records", [] {
            std::lock_guard<std::mutex> lk(g_mu);
            return static_cast<int64_t>(g_recs.size());
        });
        nxrt::gauge_register("nvn.prewarm.records.bytes", [] {
            std::lock_guard<std::mutex> lk(g_mu);
            return static_cast<int64_t>(g_recs.size() * sizeof(Rec));
        });
        nxrt::gauge_register("nvn.prewarm.digests", [] {
            std::lock_guard<std::mutex> lk(g_mu);
            return static_cast<int64_t>(g_digests.size());
        });
        nxrt::gauge_register("nvn.prewarm.queue", [] {
            std::lock_guard<std::mutex> lk(g_mu);
            return static_cast<int64_t>(g_queue.size());
        });
        nxrt::gauge_register("nvn.prewarm.jobs", [] {
            std::lock_guard<std::mutex> lk(g_mu);
            return static_cast<int64_t>(g_jobs.size());
        });
    }
    ~PrewarmGauges() {
        for (const char* n : {"nvn.prewarm.records", "nvn.prewarm.records.bytes", "nvn.prewarm.digests", "nvn.prewarm.queue", "nvn.prewarm.jobs"})
            nxrt::gauge_unregister(n);
    }
};
PrewarmGauges g_prewarm_gauges;
}  // namespace

}  // namespace nxrt::nvn::be::gpu
