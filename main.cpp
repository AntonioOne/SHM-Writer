// ShmFrameTagger: a third writer on RDB image shared memory (keys 0x8100-0x81ff).
// At 60 Hz it writes a counter into the first 4 pixels of the image in every
// buffer, so no two reads of the image are ever identical frames.
//
// Rules:
//  - Only the bytes of the first 4 pixels of the image data are written
//    (4 * bytesPerPixel, e.g. 12 bytes for RGB8). Those bytes hold one big-endian
//    unsigned integer (last byte least significant) that starts at 0 and increases
//    by one per write cycle.
//  - Other writers (VTD) are not coordinated with: every buffer is stamped every cycle.
//  - SHM headers, buffer infos and flags (LOCK / TC / IG) are never written.
//
// Usage: ShmFrameTagger [--range <lo> <hi>] [--rate <Hz>] [key ...]
// Keys:  q/ESC quit, p pause/resume writing

#include <sys/ipc.h>
#include <sys/shm.h>

#include <opencv2/core.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "rdb_shm.h"

namespace {

std::atomic<bool> gStop{false};

constexpr int kTagPixels = 4;

#ifdef TAG_OVERLAY
const char* const kAppName = "SHM Frame Tagger + Overlay";
#else
const char* const kAppName = "SHM Frame Tagger";
#endif

std::string hexKey(key_t key)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%05x", static_cast<unsigned>(key));
    return buf;
}

// Returns key -> shmid of all live segments (removed segments report key 0 and are skipped).
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

const char* pixelFormatName(int fmt)
{
    switch (fmt) {
    case RDB_PIX_FORMAT_RGB_24:   return "RGB_24";
    case RDB_PIX_FORMAT_RGBA_24:  return "RGBA_24";
    case RDB_PIX_FORMAT_RGB8:     return "RGB8";
    case RDB_PIX_FORMAT_RGBA8:    return "RGBA8";
    case RDB_PIX_FORMAT_RGB16:    return "RGB16";
    case RDB_PIX_FORMAT_RGBA16:   return "RGBA16";
    case RDB_PIX_FORMAT_RGB16F:   return "RGB16F";
    case RDB_PIX_FORMAT_RGBA16F:  return "RGBA16F";
    case RDB_PIX_FORMAT_RGB32F:   return "RGB32F";
    case RDB_PIX_FORMAT_RGBA32F:  return "RGBA32F";
    case RDB_PIX_FORMAT_RED8:     return "RED8";
    case RDB_PIX_FORMAT_BW_8:     return "BW_8";
    case RDB_PIX_FORMAT_RED16:    return "RED16";
    case RDB_PIX_FORMAT_BW_16:    return "BW_16";
    default:                      return "?";
    }
}

// Bytes per pixel; 0 if unknown.
unsigned bytesPerPixel(const RDB_IMAGE_t& img)
{
    switch (img.pixelFormat) {
    case RDB_PIX_FORMAT_RED8:
    case RDB_PIX_FORMAT_BW_8:      return 1;
    case RDB_PIX_FORMAT_RED16:
    case RDB_PIX_FORMAT_BW_16:     return 2;
    case RDB_PIX_FORMAT_RGB_24:
    case RDB_PIX_FORMAT_RGB8:      return 3;
    case RDB_PIX_FORMAT_RGBA_24:
    case RDB_PIX_FORMAT_RGBA8:     return 4;
    case RDB_PIX_FORMAT_RGB16:
    case RDB_PIX_FORMAT_RGB16F:
    case RDB_PIX_FORMAT_RGB_16_F:  return 6;
    case RDB_PIX_FORMAT_RGBA16:
    case RDB_PIX_FORMAT_RGBA16F:
    case RDB_PIX_FORMAT_RGBA_16_F: return 8;
    case RDB_PIX_FORMAT_RGB32F:
    case RDB_PIX_FORMAT_RGB_32_F:  return 12;
    case RDB_PIX_FORMAT_RGBA32F:
    case RDB_PIX_FORMAT_RGBA_32_F: return 16;
    default:
        // VTD fills pixelSize in bits (e.g. 24 for RGB8).
        if (img.pixelSize >= 8 && img.pixelSize % 8 == 0)
            return img.pixelSize / 8;
        return 0;
    }
}

struct BufferStatus {
    int index = 0;
    uint32_t flags = 0;
    uint32_t frameNo = 0;
    bool written = false;
    bool verified = false;
    uint64_t value = 0;
    uint8_t bytes[16 * kTagPixels] = {};
    size_t byteCount = 0;
    std::string note;
};

struct SegmentStatus {
    key_t key = 0;
    std::string format;
    unsigned width = 0;
    unsigned height = 0;
    unsigned bpp = 0;
    std::string error;
    std::vector<BufferStatus> buffers;
    uint64_t counter = 0;
    uint64_t writes = 0;
    uint64_t verifyFailures = 0;
    double writeRate = 0.0;
    bool overlayActive = false;
    unsigned overlayValue = 0;
};

#ifdef TAG_OVERLAY
constexpr int kOverlayW = 100;
constexpr int kOverlayH = 50;

// Overlay patch already in image memory layout: rows bottom-up (OpenGL read-back) and
// RGB channel order, so it appears upright in viewers that flip the image.
struct Overlay {
    cv::Mat rgb;
    cv::Mat mask;
    uint64_t value = UINT64_MAX;
};

void buildOverlay(Overlay& ov, uint64_t value)
{
    if (ov.value == value)
        return;

    cv::Mat bgr(kOverlayH, kOverlayW, CV_8UC3, cv::Scalar(0, 0, 0));
    cv::Mat mask(kOverlayH, kOverlayW, CV_8U, cv::Scalar(0));
    const cv::Point center(kOverlayW / 2, kOverlayH / 2);
    const cv::Size axes(kOverlayW / 2 - 1, kOverlayH / 2 - 1);
    cv::ellipse(mask, center, axes, 0, 0, 360, cv::Scalar(255), cv::FILLED, cv::LINE_8);
    cv::ellipse(bgr, center, axes, 0, 0, 360, cv::Scalar(255, 255, 255), cv::FILLED, cv::LINE_8);
    cv::ellipse(bgr, center, axes, 0, 0, 360, cv::Scalar(0, 0, 255), 2, cv::LINE_8);

    char text[8];
    std::snprintf(text, sizeof(text), "%04u", static_cast<unsigned>(value % 10000));
    int baseline = 0;
    const double scale = 0.8;
    const int thickness = 2;
    const cv::Size ts = cv::getTextSize(text, cv::FONT_HERSHEY_SIMPLEX, scale, thickness, &baseline);
    cv::putText(bgr, text, cv::Point((kOverlayW - ts.width) / 2, (kOverlayH + ts.height) / 2),
                cv::FONT_HERSHEY_SIMPLEX, scale, cv::Scalar(0, 0, 0), thickness, cv::LINE_AA);

    cv::flip(bgr, bgr, 0);
    cv::flip(mask, ov.mask, 0);
    cv::cvtColor(bgr, ov.rgb, cv::COLOR_BGR2RGB);
    ov.value = value;
}

bool isEightBitPerChannel(int fmt)
{
    switch (fmt) {
    case RDB_PIX_FORMAT_RGB_24:
    case RDB_PIX_FORMAT_RGB8:
    case RDB_PIX_FORMAT_RGBA_24:
    case RDB_PIX_FORMAT_RGBA8:
    case RDB_PIX_FORMAT_RED8:
    case RDB_PIX_FORMAT_BW_8:
        return true;
    default:
        return false;
    }
}
#endif

class Segment {
public:
    Segment(key_t key) { status_.key = key; }
    ~Segment() { detach(); }

    bool attach()
    {
        id_ = shmget(status_.key, 0, 0);
        if (id_ < 0)
            return false;
        void* p = shmat(id_, nullptr, 0);
        if (p == reinterpret_cast<void*>(-1)) {
            std::fprintf(stderr, "[%s] shmat failed: %s\n", hexKey(status_.key).c_str(), std::strerror(errno));
            id_ = -1;
            return false;
        }
        shmid_ds info{};
        if (shmctl(id_, IPC_STAT, &info) < 0) {
            shmdt(p);
            id_ = -1;
            return false;
        }
        base_ = static_cast<uint8_t*>(p);
        size_ = info.shm_segsz;
        std::fprintf(stderr, "[%s] attached (%zu bytes)\n", hexKey(status_.key).c_str(), size_);
        return true;
    }

    void detach()
    {
        if (base_) {
            shmdt(base_);
            std::fprintf(stderr, "[%s] detached\n", hexKey(status_.key).c_str());
        }
        base_ = nullptr;
        size_ = 0;
        id_ = -1;
    }

    int id() const { return id_; }
    const SegmentStatus& status() const { return status_; }

    // One write cycle: stamps every unlocked buffer that holds an image.
    void tick()
    {
        status_.error.clear();
        RDB_SHM_HDR_t hdr{};
        if (!readAt(0, hdr) || hdr.headerSize == 0 || hdr.noBuffers == 0) {
            status_.error = "no RDB SHM header";
            status_.buffers.clear();
            return;
        }

        status_.buffers.resize(hdr.noBuffers);
        bool anyWritten = false;
        for (unsigned i = 0; i < hdr.noBuffers; ++i) {
            BufferStatus& bs = status_.buffers[i];
            bs = BufferStatus{};
            bs.index = int(i);
            if (stampBuffer(hdr.headerSize + i * sizeof(RDB_SHM_BUFFER_INFO_t), bs))
                anyWritten = true;
        }

        if (anyWritten) {
            ++status_.counter;
            ++writesInWindow_;
        }

        const auto now = std::chrono::steady_clock::now();
        const double dt = std::chrono::duration<double>(now - rateStart_).count();
        if (dt >= 1.0) {
            status_.writeRate = writesInWindow_ / dt;
            writesInWindow_ = 0;
            rateStart_ = now;
        }
    }

private:
    template <typename T>
    bool readAt(size_t offset, T& out) const
    {
        if (offset + sizeof(T) > size_)
            return false;
        std::memcpy(&out, base_ + offset, sizeof(T));
        return true;
    }

    uint32_t loadFlags(size_t infoOffset) const
    {
        const auto* p = reinterpret_cast<const uint32_t*>(base_ + infoOffset + offsetof(RDB_SHM_BUFFER_INFO_t, flags));
        return __atomic_load_n(p, __ATOMIC_RELAXED);
    }

    // Finds the absolute SHM offset of the first image's pixel data in this buffer.
    bool locateImage(const RDB_SHM_BUFFER_INFO_t& info, RDB_IMAGE_t& img, size_t& dataOffset, BufferStatus& bs)
    {
        RDB_MSG_HDR_t msg{};
        if (!readAt(info.offset, msg) || msg.magicNo != RDB_MAGIC_NO) {
            bs.note = "no RDB message";
            return false;
        }
        bs.frameNo = msg.frameNo;

        const size_t msgEnd = size_t(info.offset) + msg.headerSize + msg.dataSize;
        if (msgEnd > size_ || msg.headerSize + size_t(msg.dataSize) > info.bufferSize) {
            bs.note = "message exceeds buffer";
            return false;
        }

        size_t pos = size_t(info.offset) + msg.headerSize;
        while (pos + sizeof(RDB_MSG_ENTRY_HDR_t) <= msgEnd) {
            RDB_MSG_ENTRY_HDR_t entry{};
            readAt(pos, entry);
            if (entry.headerSize == 0)
                break;
            const size_t dataPos = pos + entry.headerSize;
            if (dataPos + entry.dataSize > msgEnd)
                break;
            if (entry.pkgId == RDB_PKG_ID_IMAGE && entry.dataSize >= sizeof(RDB_IMAGE_t)) {
                readAt(dataPos, img);
                if (sizeof(RDB_IMAGE_t) + size_t(img.imgSize) > entry.dataSize) {
                    bs.note = "imgSize exceeds entry";
                    return false;
                }
                dataOffset = dataPos + sizeof(RDB_IMAGE_t);
                return true;
            }
            pos = dataPos + entry.dataSize;
        }
        bs.note = "no image package";
        return false;
    }

    bool stampBuffer(size_t infoOffset, BufferStatus& bs)
    {
        RDB_SHM_BUFFER_INFO_t info{};
        if (!readAt(infoOffset, info)) {
            bs.note = "bad buffer info";
            return false;
        }
        bs.flags = loadFlags(infoOffset);

        RDB_IMAGE_t img{};
        size_t dataOffset = 0;
        if (!locateImage(info, img, dataOffset, bs))
            return false;

        const unsigned bpp = bytesPerPixel(img);
        status_.format = pixelFormatName(img.pixelFormat);
        status_.width = img.width;
        status_.height = img.height;
        status_.bpp = bpp;
        const size_t n = size_t(bpp) * kTagPixels;
        if (bpp == 0 || img.imgSize < n || dataOffset + n > size_) {
            bs.note = "unsupported pixel format";
            return false;
        }

        // Big-endian counter over all n bytes; a 64-bit counter fills the low 8 bytes and
        // the higher bytes stay 0 (it cannot overflow at 60 Hz in practice).
        uint8_t tag[16 * kTagPixels] = {};
        bs.value = status_.counter;
        for (size_t b = 0; b < n && b < sizeof(uint64_t); ++b)
            tag[n - 1 - b] = static_cast<uint8_t>(bs.value >> (8 * b));
        std::memcpy(bs.bytes, tag, n);
        bs.byteCount = n;

        std::memcpy(base_ + dataOffset, tag, n);
        std::atomic_thread_fence(std::memory_order_seq_cst);
#ifdef TAG_OVERLAY
        buildOverlay(overlay_, status_.counter);
        if (!writeOverlay(img, dataOffset, bpp))
            bs.note = "overlay: needs 8-bit RGB/RGBA/gray, >= 100x50";
#endif

        bs.written = true;
        bs.verified = std::memcmp(base_ + dataOffset, tag, n) == 0;
        ++status_.writes;
        if (!bs.verified) {
            // Another writer overwrote the stamp right away; the next cycle stamps again.
            ++status_.verifyFailures;
            bs.note = "overwritten by another writer";
        }
        return true;
    }

#ifdef TAG_OVERLAY
    // Writes the overlay ellipse (only pixels inside it) centred in the image.
    bool writeOverlay(const RDB_IMAGE_t& img, size_t dataOffset, unsigned bpp)
    {
        if (!isEightBitPerChannel(img.pixelFormat) || img.width < kOverlayW || img.height < kOverlayH)
            return false;
        const size_t stride = img.imgSize / img.height;
        if (stride < size_t(img.width) * bpp)
            return false;

        const size_t x0 = (img.width - kOverlayW) / 2;
        const size_t y0 = (img.height - kOverlayH) / 2;
        if (dataOffset + (y0 + kOverlayH - 1) * stride + (x0 + kOverlayW) * bpp > size_)
            return false;

        for (int y = 0; y < kOverlayH; ++y) {
            uint8_t* row = base_ + dataOffset + (y0 + y) * stride + x0 * bpp;
            const uint8_t* src = overlay_.rgb.ptr<uint8_t>(y);
            const uint8_t* mask = overlay_.mask.ptr<uint8_t>(y);
            for (int x = 0; x < kOverlayW; ++x) {
                if (!mask[x])
                    continue;
                uint8_t* d = row + x * bpp;
                const uint8_t* s = src + 3 * x;
                if (bpp == 1) {
                    d[0] = static_cast<uint8_t>((s[0] * 77 + s[1] * 150 + s[2] * 29) >> 8);
                } else {
                    d[0] = s[0];
                    d[1] = s[1];
                    d[2] = s[2];
                    if (bpp == 4)
                        d[3] = 255;
                }
            }
        }
        status_.overlayValue = static_cast<unsigned>(overlay_.value % 10000);
        status_.overlayActive = true;
        return true;
    }

    Overlay overlay_;
#endif

    SegmentStatus status_;
    int id_ = -1;
    uint8_t* base_ = nullptr;
    size_t size_ = 0;
    int writesInWindow_ = 0;
    std::chrono::steady_clock::time_point rateStart_ = std::chrono::steady_clock::now();
};

struct SharedState {
    std::mutex mutex;
    std::vector<SegmentStatus> segments;
    double loopRate = 0.0;
    double maxLateMs = 0.0;
};

struct Config {
    long rangeLo = 0x8100;
    long rangeHi = 0x81ff;
    std::set<key_t> keys;
    double rateHz = 60.0;
};

void writerThread(const Config& cfg, SharedState& shared, std::atomic<bool>& paused)
{
    std::map<key_t, std::unique_ptr<Segment>> segments;
    auto wanted = [&](key_t key) {
        return cfg.keys.empty() ? (key >= cfg.rangeLo && key <= cfg.rangeHi) : cfg.keys.count(key) > 0;
    };

    using clock = std::chrono::steady_clock;
    const auto period = std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(1.0 / cfg.rateHz));
    auto next = clock::now();
    auto lastScan = clock::time_point{};
    auto rateStart = clock::now();
    int loops = 0;
    double maxLateMs = 0.0;

    while (!gStop) {
        const auto now = clock::now();
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

        if (!paused)
            for (auto& kv : segments)
                kv.second->tick();

        ++loops;
        const double dt = std::chrono::duration<double>(now - rateStart).count();
        {
            std::lock_guard<std::mutex> lock(shared.mutex);
            shared.segments.clear();
            for (auto& kv : segments)
                shared.segments.push_back(kv.second->status());
            if (dt >= 1.0) {
                shared.loopRate = loops / dt;
                shared.maxLateMs = maxLateMs;
            }
        }
        if (dt >= 1.0) {
            loops = 0;
            maxLateMs = 0.0;
            rateStart = now;
        }

        // Absolute schedule avoids drift; if we fall far behind, resynchronise.
        next += period;
        if (clock::now() - next > period * 5)
            next = clock::now();
        std::this_thread::sleep_until(next);
    }
}

void drawText(cv::Mat& img, const std::string& s, int x, int y, const cv::Scalar& color, double scale = 0.5)
{
    cv::putText(img, s, cv::Point(x, y), cv::FONT_HERSHEY_SIMPLEX, scale, color, 1, cv::LINE_AA);
}

cv::Mat renderTable(const std::vector<SegmentStatus>& segs, double loopRate, double maxLateMs, bool paused)
{
    const int rowH = 22;
    int rows = 0;
    for (const auto& s : segs)
        rows += 2 + int(std::max<size_t>(s.buffers.size(), 1));
    const int height = std::max(160, 70 + rows * rowH + 20);
    cv::Mat img(height, 1260, CV_8UC3, cv::Scalar(30, 30, 30));

    const cv::Scalar white(230, 230, 230), gray(150, 150, 150), green(80, 220, 80), red(80, 80, 255),
        yellow(60, 220, 230), cyan(230, 200, 80);

    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s   keys 0x81xx   loop %.1f Hz   max late %.2f ms   %s",
                  kAppName, loopRate, maxLateMs, paused ? "PAUSED (p to resume)" : "WRITING (p to pause, q to quit)");
    drawText(img, buf, 12, 26, paused ? yellow : white, 0.55);

    int y = 60;
    if (segs.empty()) {
        drawText(img, "No 0x81xx shared memory found - waiting ...", 12, y + 20, gray, 0.6);
        return img;
    }

    for (const auto& s : segs) {
        if (s.error.empty())
            std::snprintf(buf, sizeof(buf),
                          "%s   %ux%u %s (%u B/px, %u B written per buffer, big-endian)   next=%llu   "
                          "%.1f writes/s   writes %llu   readback fail %llu",
                          hexKey(s.key).c_str(), s.width, s.height, s.format.c_str(), s.bpp,
                          s.bpp * kTagPixels, (unsigned long long)s.counter, s.writeRate,
                          (unsigned long long)s.writes, (unsigned long long)s.verifyFailures);
        else
            std::snprintf(buf, sizeof(buf), "%s   %s", hexKey(s.key).c_str(), s.error.c_str());
        drawText(img, buf, 12, y, s.error.empty() ? cyan : red, 0.48);
        if (s.overlayActive) {
            std::snprintf(buf, sizeof(buf), "overlay %04u", s.overlayValue);
            drawText(img, buf, 1120, y, green, 0.48);
        }
        y += rowH;

        const int xBuf = 30, xFlags = 80, xFrame = 150, xValue = 260, xBytes = 400, xCheck = 820, xNote = 920;
        drawText(img, "buf", xBuf, y, gray, 0.45);
        drawText(img, "flags", xFlags, y, gray, 0.45);
        drawText(img, "VTD frame", xFrame, y, gray, 0.45);
        drawText(img, "written value", xValue, y, gray, 0.45);
        drawText(img, "written bytes (hex)", xBytes, y, gray, 0.45);
        drawText(img, "readback", xCheck, y, gray, 0.45);
        drawText(img, "note", xNote, y, gray, 0.45);
        y += rowH;

        for (const auto& b : s.buffers) {
            drawText(img, std::to_string(b.index), xBuf, y, white, 0.45);
            std::snprintf(buf, sizeof(buf), "0x%x", b.flags);
            drawText(img, buf, xFlags, y, white, 0.45);
            drawText(img, std::to_string(b.frameNo), xFrame, y, white, 0.45);
            if (b.written) {
                drawText(img, std::to_string(b.value), xValue, y, white, 0.45);
                std::string hex;
                for (size_t i = 0; i < b.byteCount; ++i) {
                    char h[4];
                    std::snprintf(h, sizeof(h), "%02X", b.bytes[i]);
                    hex += (i ? " " : "") + std::string(h);
                }
                drawText(img, hex, xBytes, y, white, 0.45);
                drawText(img, b.verified ? "OK" : "FAIL", xCheck, y, b.verified ? green : red, 0.45);
            } else {
                drawText(img, "-", xValue, y, gray, 0.45);
            }
            drawText(img, b.note, xNote, y, b.note.empty() ? gray : yellow, 0.45);
            y += rowH;
        }
        y += 8;
    }
    return img;
}

void usage(const char* prog)
{
    std::fprintf(stderr,
                 "Usage: %s [--range <lo> <hi>] [--rate <Hz>] [key ...]\n"
                 "  Default: all shared memory keys 0x8100-0x81ff at 60 Hz.\n"
                 "Keys:  q/ESC quit, p pause/resume writing\n",
                 prog);
}

bool parseKey(const char* s, long& out)
{
    char* end = nullptr;
    out = std::strtol(s, &end, 0);
    return *s != '\0' && end && *end == '\0';
}

} // namespace

int main(int argc, char** argv)
{
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        long v = 0;
        if (a == "-h" || a == "--help") {
            usage(argv[0]);
            return 0;
        } else if (a == "--rate" && i + 1 < argc) {
            cfg.rateHz = std::atof(argv[++i]);
            if (cfg.rateHz <= 0.0) {
                usage(argv[0]);
                return 1;
            }
        } else if (a == "--range" && i + 2 < argc) {
            if (!parseKey(argv[i + 1], cfg.rangeLo) || !parseKey(argv[i + 2], cfg.rangeHi)) {
                usage(argv[0]);
                return 1;
            }
            i += 2;
        } else if (parseKey(argv[i], v)) {
            cfg.keys.insert(static_cast<key_t>(v));
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    std::signal(SIGINT, [](int) { gStop = true; });
    std::signal(SIGTERM, [](int) { gStop = true; });

    SharedState shared;
    std::atomic<bool> paused{false};
    std::thread writer(writerThread, std::cref(cfg), std::ref(shared), std::ref(paused));

    const std::string title = kAppName;
    cv::namedWindow(title, cv::WINDOW_AUTOSIZE);
    while (!gStop) {
        std::vector<SegmentStatus> segs;
        double loopRate, maxLateMs;
        {
            std::lock_guard<std::mutex> lock(shared.mutex);
            segs = shared.segments;
            loopRate = shared.loopRate;
            maxLateMs = shared.maxLateMs;
        }
        cv::imshow(title, renderTable(segs, loopRate, maxLateMs, paused));
        const int k = cv::waitKey(33);
        if (k == 'q' || k == 27)
            break;
        if (k == 'p')
            paused = !paused;
        if (cv::getWindowProperty(title, cv::WND_PROP_AUTOSIZE) < 0)
            break;
    }

    gStop = true;
    writer.join();
    cv::destroyAllWindows();
    return 0;
}
