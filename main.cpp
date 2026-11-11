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
//  - Overlay build (-DTAG_OVERLAY): headless (no window). Additionally the counter's last
//    four digits (white with black outline, within a 100x50 area) are drawn in the image
//    centre; only the digit pixels are written. Writes at 200 Hz by default (counter +1 per
//    write). Optionally (--poll-us) the stamp and digits are treated as owned territory
//    between writes: checked every <us> (0 = spin) and rewritten immediately whenever another
//    writer (VTD) has overwritten them.
//  - SHM headers, buffer infos and flags (LOCK / TC / IG) are never written.
//
// Usage: ShmFrameTagger [--range <lo> <hi>] [--rate <Hz>] [--poll-us <us>] [key ...]
// Keys:  q/ESC quit, p pause/resume writing

#include <sys/ipc.h>
#include <sys/prctl.h>
#include <sys/shm.h>

#include <opencv2/core.hpp>
#ifndef TAG_OVERLAY
#include <opencv2/highgui.hpp>
#endif
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
    uint64_t reclaims = 0;
    double reclaimRate = 0.0;
    uint64_t forcedCaptures = 0;
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
    int firstRow = 0;
    int lastRow = kOverlayH - 1;
    // Per row, the contiguous runs [first, last] of overlay pixels.
    std::vector<std::pair<int, int>> spans[kOverlayH];
};

// One patch shared by all segments (they all write the same counter value); writer thread only.
Overlay& sharedOverlay()
{
    static Overlay overlay;
    return overlay;
}

void buildOverlay(Overlay& ov, uint64_t value)
{
    if (ov.value == value)
        return;

    // Only the digits are drawn: white glyphs on a black outline (readable on any background).
    // Only pixels of glyph + outline are written to the image.
    cv::Mat bgr(kOverlayH, kOverlayW, CV_8UC3, cv::Scalar(0, 0, 0));
    cv::Mat mask(kOverlayH, kOverlayW, CV_8U, cv::Scalar(0));

    char text[8];
    std::snprintf(text, sizeof(text), "%04u", static_cast<unsigned>(value % 10000));
    int baseline = 0;
    const double scale = 0.9;
    const int thickness = 2;
    const int outline = 5;
    const cv::Size ts = cv::getTextSize(text, cv::FONT_HERSHEY_SIMPLEX, scale, thickness, &baseline);
    const cv::Point org((kOverlayW - ts.width) / 2, (kOverlayH + ts.height) / 2);
    cv::putText(mask, text, org, cv::FONT_HERSHEY_SIMPLEX, scale, cv::Scalar(255), outline, cv::LINE_8);
    cv::putText(bgr, text, org, cv::FONT_HERSHEY_SIMPLEX, scale, cv::Scalar(255, 255, 255), thickness, cv::LINE_8);

    cv::flip(bgr, bgr, 0);
    cv::flip(mask, ov.mask, 0);
    cv::cvtColor(bgr, ov.rgb, cv::COLOR_BGR2RGB);
    ov.value = value;
    ov.firstRow = -1;
    for (int y = 0; y < kOverlayH; ++y) {
        const uint8_t* m = ov.mask.ptr<uint8_t>(y);
        auto& spans = ov.spans[y];
        spans.clear();
        for (int x = 0; x < kOverlayW; ++x) {
            if (!m[x])
                continue;
            if (!spans.empty() && spans.back().second == x - 1)
                spans.back().second = x;
            else
                spans.emplace_back(x, x);
        }
        if (!spans.empty()) {
            if (ov.firstRow < 0)
                ov.firstRow = y;
            ov.lastRow = y;
        }
    }
    if (ov.firstRow < 0)
        ov.firstRow = ov.lastRow = 0;
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

    // One write cycle: stamps every buffer that holds an image with `value`.
    // Returns true if anything was written.
    bool tick(uint64_t value)
    {
        value_ = value;
        status_.error.clear();
        RDB_SHM_HDR_t hdr{};
        if (!readAt(0, hdr) || hdr.headerSize == 0 || hdr.noBuffers == 0) {
            status_.error = "no RDB SHM header";
            status_.buffers.clear();
            return false;
        }

        status_.buffers.resize(hdr.noBuffers);
#ifdef TAG_OVERLAY
        territories_.resize(hdr.noBuffers);
#endif
        bool anyWritten = false;
        for (unsigned i = 0; i < hdr.noBuffers; ++i) {
            BufferStatus& bs = status_.buffers[i];
            bs = BufferStatus{};
            bs.index = int(i);
            if (stampBuffer(hdr.headerSize + i * sizeof(RDB_SHM_BUFFER_INFO_t), bs))
                anyWritten = true;
        }

        if (anyWritten) {
            status_.counter = value + 1;
            ++writesInWindow_;
        }

        const auto now = std::chrono::steady_clock::now();
        const double dt = std::chrono::duration<double>(now - rateStart_).count();
        if (dt >= 1.0) {
            status_.writeRate = writesInWindow_ / dt;
            writesInWindow_ = 0;
#ifdef TAG_OVERLAY
            status_.reclaimRate = reclaimsInWindow_ / dt;
            reclaimsInWindow_ = 0;
#endif
            rateStart_ = now;
        }
        return anyWritten;
    }

#ifdef TAG_OVERLAY
    // Between cycles: rewrites the stamp and digits of any buffer whose territory another
    // writer has touched.
    void guard()
    {
        for (auto& t : territories_) {
            if (!t.valid)
                continue;
            const bool tagChanged = std::memcmp(base_ + t.dataOffset, t.tag, t.n) != 0;
            const bool areaChanged = t.overlay && t.hasBg && !areaIntact(t);
            if (!tagChanged && !areaChanged)
                continue;
            if (tagChanged)
                std::memcpy(base_ + t.dataOffset, t.tag, t.n);
            if (t.overlay)
                refreshOverlay(t, currentFrameNo(t), tagChanged);
            ++status_.reclaims;
            ++reclaimsInWindow_;
        }
    }
#endif

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
        bs.value = value_;
        for (size_t b = 0; b < n && b < sizeof(uint64_t); ++b)
            tag[n - 1 - b] = static_cast<uint8_t>(bs.value >> (8 * b));
        std::memcpy(bs.bytes, tag, n);
        bs.byteCount = n;

#ifdef TAG_OVERLAY
        Territory* terr = size_t(bs.index) < territories_.size() ? &territories_[bs.index] : nullptr;
        if (terr && (terr->dataOffset != dataOffset || terr->bpp != bpp || terr->n != n ||
                     terr->img.width != img.width || terr->img.height != img.height ||
                     terr->img.imgSize != img.imgSize))
            *terr = Territory{};
        // Our previous stamp is gone: the producer has started (or finished) writing a new frame.
        const bool stampGone = terr && terr->valid && std::memcmp(base_ + dataOffset, terr->tag, n) != 0;
#endif

        std::memcpy(base_ + dataOffset, tag, n);
        std::atomic_thread_fence(std::memory_order_seq_cst);
#ifdef TAG_OVERLAY
        if (terr) {
            terr->valid = true;
            terr->msgOffset = info.offset;
            terr->dataOffset = dataOffset;
            terr->bpp = bpp;
            terr->img = img;
            terr->n = n;
            std::memcpy(terr->tag, tag, n);
            terr->overlay = overlaySupported(img, dataOffset, bpp);
            if (!terr->overlay) {
                bs.note = "digits: needs 8-bit RGB/RGBA/gray, >= 100x50";
            } else {
                buildOverlay(overlay_, value_);
                if (!refreshOverlay(*terr, bs.frameNo, stampGone))
                    bs.note = "digits deferred: producer is copying";
            }
        }
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
    struct Territory {
        bool valid = false;
        bool overlay = false;
        size_t dataOffset = 0;
        unsigned bpp = 0;
        RDB_IMAGE_t img{};
        size_t n = 0;
        uint8_t tag[16 * kTagPixels] = {};
        // The 100x50 centre area: producer pixels saved after its last full write (background)
        // and what we wrote last (background + digits).
        bool hasBg = false;
        std::vector<uint8_t> bg;
        std::vector<uint8_t> written;
        size_t msgOffset = 0;
        uint32_t bgFrameNo = 0;                              // frame the background was taken from
        std::chrono::steady_clock::time_point deferredSince; // epoch = not deferring
    };

    uint32_t currentFrameNo(const Territory& t) const
    {
        RDB_MSG_HDR_t msg{};
        readAt(t.msgOffset, msg);
        return msg.frameNo;
    }

    bool overlaySupported(const RDB_IMAGE_t& img, size_t dataOffset, unsigned bpp) const
    {
        if (!isEightBitPerChannel(img.pixelFormat) || img.width < kOverlayW || img.height < kOverlayH)
            return false;
        const size_t stride = img.imgSize / img.height;
        if (stride < size_t(img.width) * bpp)
            return false;
        const size_t x0 = (img.width - kOverlayW) / 2;
        const size_t y0 = (img.height - kOverlayH) / 2;
        return dataOffset + (y0 + kOverlayH - 1) * stride + (x0 + kOverlayW) * bpp <= size_;
    }

    uint8_t* areaRow(const Territory& t, int y) const
    {
        const size_t stride = t.img.imgSize / t.img.height;
        const size_t x0 = (t.img.width - kOverlayW) / 2;
        const size_t y0 = (t.img.height - kOverlayH) / 2;
        return base_ + t.dataOffset + (y0 + y) * stride + x0 * t.bpp;
    }

    // True if the whole centre area still holds exactly what we wrote last.
    bool areaIntact(const Territory& t) const
    {
        const size_t rowBytes = size_t(kOverlayW) * t.bpp;
        if (t.written.size() != rowBytes * kOverlayH)
            return false;
        for (int y = 0; y < kOverlayH; ++y)
            if (std::memcmp(areaRow(t, y), t.written.data() + y * rowBytes, rowBytes) != 0)
                return false;
        return true;
    }

    // Writes background + current digits over the whole centre area, so digits of the previous
    // value are erased back to the producer's pixels (no ghosting, no visible box).
    //  - nothing touched since our last write: reuse the saved background;
    //  - a new frame number is published: the producer finished the frame, its pixels become
    //    the new background;
    //  - stamp or area changed but same frame number: the producer is still copying -> defer
    //    (returns false), unless that lasts > 50 ms (producer that publishes before copying).
    bool refreshOverlay(Territory& t, uint32_t frameNo, bool stampGone)
    {
        using clock = std::chrono::steady_clock;
        const size_t rowBytes = size_t(kOverlayW) * t.bpp;
        bool capture = !t.hasBg;
        if (!capture && (stampGone || frameNo != t.bgFrameNo || !areaIntact(t))) {
            if (frameNo != t.bgFrameNo) {
                capture = true;
            } else {
                const auto now = clock::now();
                if (t.deferredSince == clock::time_point{})
                    t.deferredSince = now;
                if (now - t.deferredSince < std::chrono::milliseconds(50))
                    return false;
                capture = true;
                ++status_.forcedCaptures;
            }
        }
        if (capture) {
            t.bg.resize(rowBytes * kOverlayH);
            for (int y = 0; y < kOverlayH; ++y)
                std::memcpy(t.bg.data() + y * rowBytes, areaRow(t, y), rowBytes);
            t.hasBg = true;
            t.bgFrameNo = frameNo;
        }
        t.deferredSince = clock::time_point{};

        std::vector<uint8_t> next = t.bg;
        for (int y = 0; y < kOverlayH; ++y) {
            uint8_t* row = next.data() + y * rowBytes;
            const uint8_t* src = overlay_.rgb.ptr<uint8_t>(y);
            for (const auto& span : overlay_.spans[y]) {
                for (int x = span.first; x <= span.second; ++x) {
                    uint8_t* d = row + x * t.bpp;
                    const uint8_t* s = src + 3 * x;
                    if (t.bpp == 1) {
                        d[0] = static_cast<uint8_t>((s[0] * 77 + s[1] * 150 + s[2] * 29) >> 8);
                    } else {
                        d[0] = s[0];
                        d[1] = s[1];
                        d[2] = s[2];
                        if (t.bpp == 4)
                            d[3] = 255;
                    }
                }
            }
        }
        // A producer copies the image front to back, so it overwrites our stamp at the image start
        // long before it reaches the centre. If the stamp is already gone, a new frame copy has
        // started: writing the old background now could mix into the new frame, so skip.
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (std::memcmp(base_ + t.dataOffset, t.tag, t.n) != 0)
            return false;
        for (int y = 0; y < kOverlayH; ++y)
            std::memcpy(areaRow(t, y), next.data() + y * rowBytes, rowBytes);
        t.written.swap(next);

        status_.overlayValue = static_cast<unsigned>(overlay_.value % 10000);
        status_.overlayActive = true;
        return true;
    }

    Overlay& overlay_ = sharedOverlay();
    std::vector<Territory> territories_;
    int reclaimsInWindow_ = 0;
#endif

    SegmentStatus status_;
    uint64_t value_ = 0;
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
#ifdef TAG_OVERLAY
    double rateHz = 200.0;
#else
    double rateHz = 60.0;
#endif
    int pollUs = -1; // overlay build: < 0 disables the territory guard
};

void writerThread(const Config& cfg, SharedState& shared, std::atomic<bool>& paused)
{
#ifdef TAG_OVERLAY
    // Default 50 us timer slack would make the short polling sleeps much longer.
    prctl(PR_SET_TIMERSLACK, 1000UL);
#endif
    std::map<key_t, std::unique_ptr<Segment>> segments;
    auto wanted = [&](key_t key) {
        return cfg.keys.empty() ? (key >= cfg.rangeLo && key <= cfg.rangeHi) : cfg.keys.count(key) > 0;
    };

    using clock = std::chrono::steady_clock;
    const auto period = std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(1.0 / cfg.rateHz));
    auto next = clock::now();
    auto lastScan = clock::time_point{};
    auto lastPublish = clock::time_point{};
    auto rateStart = clock::now();
    uint64_t counter = 0;
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

        if (!paused) {
            bool anyWritten = false;
            for (auto& kv : segments)
                anyWritten = kv.second->tick(counter) || anyWritten;
            if (anyWritten)
                ++counter;
        }

        ++loops;
        const double dt = std::chrono::duration<double>(now - rateStart).count();
        // The UI only needs a few updates per second; copying every cycle wastes CPU at 300 Hz.
        if (now - lastPublish >= std::chrono::milliseconds(50) || dt >= 1.0) {
            lastPublish = now;
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
#ifdef TAG_OVERLAY
        if (cfg.pollUs < 0) {
            std::this_thread::sleep_until(next);
            continue;
        }
        while (!gStop && clock::now() < next) {
            if (!paused)
                for (auto& kv : segments)
                    kv.second->guard();
            if (cfg.pollUs > 0)
                std::this_thread::sleep_for(std::chrono::microseconds(cfg.pollUs));
            else
                __builtin_ia32_pause(); // spin: catches an overwrite while the producer is still copying
        }
#else
        std::this_thread::sleep_until(next);
#endif
    }
}

#ifndef TAG_OVERLAY
void drawText(cv::Mat& img, const std::string& s, int x, int y, const cv::Scalar& color, double scale = 0.5)
{
    cv::putText(img, s, cv::Point(x, y), cv::FONT_HERSHEY_SIMPLEX, scale, color, 1, cv::LINE_AA);
}

cv::Mat renderTable(const std::vector<SegmentStatus>& segs, double loopRate, double maxLateMs, bool paused)
{
    const int rowH = 22;
    int rows = 0;
    for (const auto& s : segs)
        rows += 2 + (s.overlayActive ? 1 : 0) + int(std::max<size_t>(s.buffers.size(), 1));
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
        y += rowH;
        if (s.overlayActive) {
            std::snprintf(buf, sizeof(buf), "overlay %04u   reclaimed after another writer overwrote it: %llu (%.1f/s)",
                          s.overlayValue, (unsigned long long)s.reclaims, s.reclaimRate);
            drawText(img, buf, 30, y, green, 0.48);
            y += rowH;
        }

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

#endif

void usage(const char* prog)
{
    std::fprintf(stderr,
                 "Usage: %s [--range <lo> <hi>] [--rate <Hz>] [--poll-us <us>] [key ...]\n"
#ifdef TAG_OVERLAY
                 "  Default: all shared memory keys 0x8100-0x81ff at 200 Hz, counter +1 per write.\n"
                 "  --poll-us: enable the territory guard between writes, checking every <us>\n"
                 "             (0 = spin, one CPU core busy). Off by default.\n"
#else
                 "  Default: all shared memory keys 0x8100-0x81ff at 60 Hz.\n"
#endif
#ifdef TAG_OVERLAY
                 "  Runs without a window; stop with Ctrl+C.\n",
#else
                 "Keys:  q/ESC quit, p pause/resume writing\n",
#endif
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
        } else if (a == "--poll-us" && i + 1 < argc) {
            cfg.pollUs = std::atoi(argv[++i]);
            if (cfg.pollUs < 0) {
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

#ifdef TAG_OVERLAY
    // Headless: one summary line every 10 s; stop with Ctrl+C / SIGTERM.
    std::fprintf(stderr, "%s: %.0f Hz, keys %s-%s%s, Ctrl+C to stop\n", kAppName, cfg.rateHz,
                 hexKey(cfg.rangeLo).c_str(), hexKey(cfg.rangeHi).c_str(),
                 cfg.pollUs >= 0 ? ", territory guard on" : "");
    auto lastReport = std::chrono::steady_clock::now();
    while (!gStop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (std::chrono::steady_clock::now() - lastReport < std::chrono::seconds(10))
            continue;
        lastReport = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(shared.mutex);
        uint64_t next = 0, fails = 0, reclaims = 0, forced = 0;
        double rate = 0.0;
        for (const auto& s : shared.segments) {
            next = std::max(next, s.counter);
            fails += s.verifyFailures;
            reclaims += s.reclaims;
            forced += s.forcedCaptures;
            rate = std::max(rate, s.writeRate);
        }
        std::fprintf(stderr, "[stats] %zu segment(s)  value %llu (digits %04llu)  %.1f writes/s  max late %.2f ms  "
                             "readback fail %llu  reclaims %llu  forced bg captures %llu\n",
                     shared.segments.size(), (unsigned long long)next, (unsigned long long)(next % 10000), rate,
                     shared.maxLateMs, (unsigned long long)fails, (unsigned long long)reclaims,
                     (unsigned long long)forced);
    }
#else
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
        const int k = cv::waitKey(100);
        if (k == 'q' || k == 27)
            break;
        if (k == 'p')
            paused = !paused;
        if (cv::getWindowProperty(title, cv::WND_PROP_AUTOSIZE) < 0)
            break;
    }
#endif

    gStop = true;
    writer.join();
#ifndef TAG_OVERLAY
    cv::destroyAllWindows();
#endif
    return 0;
}
