// ShmFrameTagger: a third writer on VTD RDB image shared memory.
//
// For every shared memory segment whose key lies in a range (default 0x8100-0x81ff) and that
// holds RDB images, a four-digit counter (0000-9999, white with black outline, 25x9 pixels) is
// drawn in the top-right corner of the displayed image of every buffer. The counter increases
// by one per write cycle (--rate, default 200 Hz), so consecutive reads of an image are never
// identical, even when VTD stalls.
//
// Between cycles the stamp area is checked every --poll-us (default 50 us); whatever VTD has
// written over it is merged into the saved background and the digits are redrawn as soon as the
// area is stable. Right after VTD publishes a new frame number (it copies the pixels ~0.2 ms
// later) the buffer is polled continuously until the stamp is restored, so it is missing only
// for microseconds.
// Only the 25x9 stamp area is written; SHM headers, buffer infos and flags are never touched.

#include <sched.h>
#include <sys/prctl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <thread>

#include "digits.h"
#include "segment.h"
#include "single_instance.h"

namespace {

using clock_type = std::chrono::steady_clock;

std::atomic<bool> gStop{false};

struct Config {
    long rangeLo = 0x8100;
    long rangeHi = 0x81ff;
    std::set<key_t> keys;
    double rateHz = 200.0;
    int pollUs = 50;
    int statsSec = 10;
    bool realtime = false;
    bool stop = false;
};

std::string hexKey(key_t key)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%05x", static_cast<unsigned>(key));
    return buf;
}

// key -> shmid of all live System V segments (removed segments report key 0).
std::map<key_t, int> listShmSegments()
{
    std::map<key_t, int> result;
    std::ifstream f("/proc/sysvipc/shm");
    std::string line;
    std::getline(f, line); // header
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        long long key = 0;
        int shmid = -1;
        if (ss >> key >> shmid && key != 0)
            result[static_cast<key_t>(key)] = shmid;
    }
    return result;
}

bool parseNumber(const char* s, long& out)
{
    char* end = nullptr;
    out = std::strtol(s, &end, 0);
    return *s != '\0' && end && *end == '\0';
}

void usage(const char* prog)
{
    std::fprintf(stderr,
                 "Usage: %s [--rate <Hz>] [--poll-us <us>] [--realtime] [--range <lo> <hi>] [--stats <s>] [key ...]\n"
                 "       %s --stop\n"
                 "  Draws a 0000-9999 counter (25x9 px) in the top-right corner of every RDB image in\n"
                 "  shared memory keys 0x8100-0x81ff (or the given keys / range).\n"
                 "  --rate     write cycles per second, counter +1 per cycle (default 200)\n"
                 "  --poll-us  check interval for VTD overwrites between cycles (default 50;\n"
                 "             0 = spin, one CPU core busy; -1 = off)\n"
                 "  --stats    seconds between status lines, 0 = off (default 10)\n"
                 "  --realtime run with real-time priority (SCHED_FIFO 50) so the stamp is restored\n"
                 "             within microseconds even under load; needs root or cap_sys_nice\n"
                 "  --stop     stop the running instance (also when it runs in the background) and exit\n"
                 "  Only one instance runs at a time: starting it again stops the running one first.\n"
                 "  Stop with Ctrl+C or --stop; the digits are removed from the images on exit.\n",
                 prog, prog);
}

bool parseArgs(int argc, char** argv, Config& cfg)
{
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        long v = 0;
        if (a == "-h" || a == "--help") {
            usage(argv[0]);
            std::exit(0);
        } else if (a == "--rate" && i + 1 < argc) {
            cfg.rateHz = std::atof(argv[++i]);
            if (cfg.rateHz <= 0.0)
                return false;
        } else if (a == "--poll-us" && i + 1 < argc) {
            if (!parseNumber(argv[++i], v))
                return false;
            cfg.pollUs = int(v);
        } else if (a == "--stats" && i + 1 < argc) {
            if (!parseNumber(argv[++i], v) || v < 0)
                return false;
            cfg.statsSec = int(v);
        } else if (a == "--stop") {
            cfg.stop = true;
        } else if (a == "--realtime") {
            cfg.realtime = true;
        } else if (a == "--range" && i + 2 < argc) {
            if (!parseNumber(argv[i + 1], cfg.rangeLo) || !parseNumber(argv[i + 2], cfg.rangeHi))
                return false;
            i += 2;
        } else if (parseNumber(argv[i], v)) {
            cfg.keys.insert(static_cast<key_t>(v));
        } else {
            return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    Config cfg;
    if (!parseArgs(argc, argv, cfg)) {
        usage(argv[0]);
        return 1;
    }
    if (cfg.stop)
        return SingleInstance::stopRunning("ShmFrameTagger");

    std::signal(SIGINT, [](int) { gStop = true; });
    std::signal(SIGTERM, [](int) { gStop = true; });
    std::signal(SIGHUP, [](int) { gStop = true; });

    SingleInstance instance("ShmFrameTagger");
    if (!instance.acquire())
        return 1;
    // The default 50 us timer slack would stretch the short polling sleeps considerably.
    prctl(PR_SET_TIMERSLACK, 1000UL);
    if (cfg.realtime) {
        sched_param sp{};
        sp.sched_priority = 50;
        if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0)
            std::fprintf(stderr, "warning: --realtime failed (%s); run as root or after "
                                 "'sudo setcap cap_sys_nice+ep ShmFrameTagger'. Continuing with normal priority.\n",
                         std::strerror(errno));
    }

    std::string keys;
    if (cfg.keys.empty())
        keys = hexKey(cfg.rangeLo) + "-" + hexKey(cfg.rangeHi);
    for (key_t k : cfg.keys)
        keys += (keys.empty() ? "" : " ") + hexKey(k);
    std::fprintf(stderr, "ShmFrameTagger: %.0f Hz, keys %s, overwrite check %s%s, Ctrl+C to stop\n", cfg.rateHz,
                 keys.c_str(), cfg.pollUs < 0 ? "off" : ("every " + std::to_string(cfg.pollUs) + " us").c_str(),
                 sched_getscheduler(0) == SCHED_FIFO ? ", real-time priority" : "");

    auto wanted = [&](key_t key) {
        return cfg.keys.empty() ? (key >= cfg.rangeLo && key <= cfg.rangeHi) : cfg.keys.count(key) > 0;
    };

    std::map<key_t, std::unique_ptr<Segment>> segments;
    const auto period = std::chrono::duration_cast<clock_type::duration>(std::chrono::duration<double>(1.0 / cfg.rateHz));
    auto next = clock_type::now();
    auto lastScan = clock_type::time_point{};
    auto lastStats = clock_type::now();
    uint64_t counter = 0;
    uint64_t cyclesSinceStats = 0;
    double maxLateMs = 0.0;
    digits::Stamp stamp = digits::render(counter);

    while (!gStop) {
        const auto now = clock_type::now();
        maxLateMs = std::max(maxLateMs, std::chrono::duration<double, std::milli>(now - next).count());

        if (now - lastScan >= std::chrono::milliseconds(500)) {
            lastScan = now;
            const auto live = listShmSegments();
            for (auto it = segments.begin(); it != segments.end();) {
                const auto seg = live.find(it->first);
                if (seg == live.end() || seg->second != it->second->id())
                    it = segments.erase(it);
                else
                    ++it;
            }
            for (const auto& seg : live) {
                if (!wanted(seg.first) || segments.count(seg.first))
                    continue;
                std::unique_ptr<Segment> s(new Segment(seg.first));
                if (s->attach())
                    segments[seg.first] = std::move(s);
            }
        }

        stamp = digits::render(counter);
        for (auto& kv : segments)
            kv.second->tick(stamp);
        ++counter;
        ++cyclesSinceStats;

        if (cfg.statsSec > 0 && now - lastStats >= std::chrono::seconds(cfg.statsSec)) {
            const double dt = std::chrono::duration<double>(now - lastStats).count();
            uint64_t writes = 0, restores = 0;
            for (auto& kv : segments) {
                writes += kv.second->stats().writes;
                restores += kv.second->stats().restores;
            }
            std::fprintf(stderr, "[stats] %zu segment(s)  digits %04llu  %.1f cycles/s  max late %.2f ms  "
                                 "writes %llu  restored after VTD overwrite %llu\n",
                         segments.size(), (unsigned long long)((counter - 1) % 10000), cyclesSinceStats / dt,
                         maxLateMs, (unsigned long long)writes, (unsigned long long)restores);
            for (auto& kv : segments)
                if (!kv.second->stats().problem.empty())
                    std::fprintf(stderr, "[%s] %s: %s\n", hexKey(kv.first).c_str(), kv.second->stats().image.c_str(),
                                 kv.second->stats().problem.c_str());
            lastStats = now;
            cyclesSinceStats = 0;
            maxLateMs = 0.0;
        }

        // Absolute schedule avoids drift; if we fall far behind, resynchronise.
        next += period;
        if (clock_type::now() - next > period * 5)
            next = clock_type::now();
        if (cfg.pollUs < 0) {
            std::this_thread::sleep_until(next);
            continue;
        }
        while (!gStop && clock_type::now() < next) {
            bool hot = false;
            for (auto& kv : segments)
                hot = kv.second->guard(stamp) || hot;
            // Right after VTD publishes a frame, poll without sleeping until the stamp is back.
            if (cfg.pollUs > 0 && !hot)
                std::this_thread::sleep_for(std::chrono::microseconds(cfg.pollUs));
            else
                __builtin_ia32_pause();
        }
    }

    for (auto& kv : segments)
        kv.second->removeStamps();
    std::fprintf(stderr, "ShmFrameTagger stopped, digits removed\n");
    segments.clear();
    return 0;
}
