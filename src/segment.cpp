#include "segment.h"

#include <sys/ipc.h>
#include <sys/shm.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace {

using clock_type = std::chrono::steady_clock;

// The producer copies a frame over the stamp area in microseconds; once no new byte has
// arrived for this long, the area is considered stable and the stamp is redrawn.
constexpr auto kSettle = std::chrono::microseconds(3);
// After a new frame number, poll continuously for at most this long (VTD publishes the frame
// number ~0.2 ms before it copies the pixels).
constexpr auto kHotWindow = std::chrono::milliseconds(5);

const char* formatName(int fmt)
{
    switch (fmt) {
    case RDB_PIX_FORMAT_RGB8:    return "RGB8";
    case RDB_PIX_FORMAT_RGB_24:  return "RGB_24";
    case RDB_PIX_FORMAT_RGBA8:   return "RGBA8";
    case RDB_PIX_FORMAT_RGBA_24: return "RGBA_24";
    case RDB_PIX_FORMAT_RED8:    return "RED8";
    case RDB_PIX_FORMAT_BW_8:    return "BW_8";
    default:                     return nullptr;
    }
}

// Bytes per pixel of the supported 8-bit-per-channel formats, 0 otherwise.
unsigned bytesPerPixel(int fmt)
{
    switch (fmt) {
    case RDB_PIX_FORMAT_RED8:
    case RDB_PIX_FORMAT_BW_8:     return 1;
    case RDB_PIX_FORMAT_RGB8:
    case RDB_PIX_FORMAT_RGB_24:   return 3;
    case RDB_PIX_FORMAT_RGBA8:
    case RDB_PIX_FORMAT_RGBA_24:  return 4;
    default:                      return 0;
    }
}

std::string hexKey(key_t key)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%05x", static_cast<unsigned>(key));
    return buf;
}

} // namespace

Segment::Segment(key_t key) : key_(key) {}

Segment::~Segment()
{
    if (base_) {
        shmdt(base_);
        std::fprintf(stderr, "[%s] detached\n", hexKey(key_).c_str());
    }
}

bool Segment::attach()
{
    id_ = shmget(key_, 0, 0);
    if (id_ < 0)
        return false;
    void* p = shmat(id_, nullptr, 0);
    if (p == reinterpret_cast<void*>(-1)) {
        std::fprintf(stderr, "[%s] shmat failed: %s\n", hexKey(key_).c_str(), std::strerror(errno));
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
    std::fprintf(stderr, "[%s] attached (%zu bytes)\n", hexKey(key_).c_str(), size_);
    return true;
}

template <typename T>
bool Segment::readAt(size_t offset, T& out) const
{
    if (offset + sizeof(T) > size_)
        return false;
    std::memcpy(&out, base_ + offset, sizeof(T));
    return true;
}

bool Segment::locateImage(const RDB_SHM_BUFFER_INFO_t& info, RDB_IMAGE_t& img, size_t& dataOffset) const
{
    RDB_MSG_HDR_t msg{};
    if (!readAt(info.offset, msg) || msg.magicNo != RDB_MAGIC_NO)
        return false;
    const size_t msgEnd = size_t(info.offset) + msg.headerSize + msg.dataSize;
    if (msgEnd > size_ || msg.headerSize + size_t(msg.dataSize) > info.bufferSize)
        return false;

    size_t pos = size_t(info.offset) + msg.headerSize;
    while (pos + sizeof(RDB_MSG_ENTRY_HDR_t) <= msgEnd) {
        RDB_MSG_ENTRY_HDR_t entry{};
        readAt(pos, entry);
        if (entry.headerSize == 0)
            return false;
        const size_t dataPos = pos + entry.headerSize;
        if (dataPos + entry.dataSize > msgEnd)
            return false;
        if (entry.pkgId == RDB_PKG_ID_IMAGE && entry.dataSize >= sizeof(RDB_IMAGE_t)) {
            readAt(dataPos, img);
            if (sizeof(RDB_IMAGE_t) + size_t(img.imgSize) > entry.dataSize)
                return false;
            dataOffset = dataPos + sizeof(RDB_IMAGE_t);
            return true;
        }
        pos = dataPos + entry.dataSize;
    }
    return false;
}

// VTD rewrites the message header when it publishes a frame, so the image lookup can fail for
// a moment. Keep guarding the last known area meanwhile; give up only if it keeps failing.
void Segment::lookupFailed(Area& a, const std::string& problem)
{
    const auto now = clock_type::now();
    if (a.failingSince == clock_type::time_point{})
        a.failingSince = now;
    if (a.valid && now - a.failingSince < std::chrono::seconds(1))
        return;
    a.valid = false;
    if (stats_.problem.empty())
        stats_.problem = problem;
}

// Re-reads the buffer layout and updates the stamp area.
void Segment::updateArea(size_t index, size_t infoOffset)
{
    Area& a = areas_[index];
    RDB_SHM_BUFFER_INFO_t info{};
    RDB_IMAGE_t img{};
    size_t dataOffset = 0;
    if (!readAt(infoOffset, info) || !locateImage(info, img, dataOffset)) {
        lookupFailed(a, "no RDB image");
        return;
    }

    const unsigned bpp = bytesPerPixel(img.pixelFormat);
    const size_t stride = img.height ? img.imgSize / img.height : 0;
    char desc[64];
    std::snprintf(desc, sizeof(desc), "%ux%u %s", unsigned(img.width), unsigned(img.height),
                  formatName(img.pixelFormat) ? formatName(img.pixelFormat) : "unsupported format");
    stats_.image = desc;
    if (bpp == 0) {
        lookupFailed(a, "pixel format " + std::to_string(img.pixelFormat) + " not supported (8-bit RGB/RGBA/gray only)");
        return;
    }
    if (img.width < digits::kWidth || img.height < digits::kHeight || stride < size_t(img.width) * bpp ||
        dataOffset + size_t(img.height) * stride > size_) {
        lookupFailed(a, "image too small or inconsistent");
        return;
    }
    a.failingSince = clock_type::time_point{};

    if (!a.valid || a.dataOffset != dataOffset || a.width != img.width || a.height != img.height ||
        a.bpp != bpp || a.stride != stride)
        a = Area{};
    a.valid = true;
    a.msgOffset = info.offset;
    a.dataOffset = dataOffset;
    a.width = img.width;
    a.height = img.height;
    a.bpp = bpp;
    a.stride = stride;
}

// Area row `row` in memory order (0 = lowest address). The image is stored bottom-up, so the
// last rows in memory are the top of the displayed picture.
uint8_t* Segment::areaRow(const Area& a, int row) const
{
    const size_t y = a.height - digits::kHeight + row;
    const size_t x = a.width - digits::kWidth;
    return base_ + a.dataOffset + y * a.stride + x * a.bpp;
}

uint32_t Segment::frameNo(const Area& a) const
{
    RDB_MSG_HDR_t msg{};
    readAt(a.msgOffset, msg);
    return msg.frameNo;
}

void Segment::captureBackground(Area& a)
{
    const size_t rowBytes = size_t(digits::kWidth) * a.bpp;
    a.bg.resize(rowBytes * digits::kHeight);
    for (int r = 0; r < digits::kHeight; ++r)
        std::memcpy(a.bg.data() + r * rowBytes, areaRow(a, r), rowBytes);
    a.seen = a.bg;
    a.hasBg = true;
}

// Every byte of the stamp area that differs from what we last wrote or observed was written by
// the producer: take it into the background (and remember it as seen). Bytes the producer has
// not reached yet keep their background value, so this converges to the producer's frame
// whatever order it copies in (forward, backward, block-wise, head/tail last).
// Returns true if the producer changed anything since the last call.
bool Segment::mergeProducerWrites(Area& a)
{
    const size_t rowBytes = size_t(digits::kWidth) * a.bpp;
    bool changed = false;
    for (int r = 0; r < digits::kHeight; ++r) {
        const uint8_t* shm = areaRow(a, r);
        uint8_t* seen = a.seen.data() + r * rowBytes;
        if (std::memcmp(shm, seen, rowBytes) == 0)
            continue;
        uint8_t* bg = a.bg.data() + r * rowBytes;
        for (size_t i = 0; i < rowBytes; ++i) {
            if (shm[i] != seen[i]) {
                bg[i] = shm[i];
                seen[i] = shm[i];
            }
        }
        changed = true;
    }
    return changed;
}

// Writes the whole 25x9 area as background + digits, so old digits never remain and no box is
// visible.
void Segment::draw(Area& a, const digits::Stamp& stamp)
{
    const int rows = digits::kHeight;
    const size_t rowBytes = size_t(digits::kWidth) * a.bpp;
    std::vector<uint8_t> next = a.bg;
    for (int r = 0; r < rows; ++r) {
        const int displayRow = rows - 1 - r; // bottom-up storage: flip so digits read upright
        uint8_t* row = next.data() + r * rowBytes;
        for (int x = 0; x < digits::kWidth; ++x) {
            const uint8_t p = stamp[displayRow * digits::kWidth + x];
            if (p == digits::kNone)
                continue;
            const uint8_t v = p == digits::kWhite ? 255 : 0;
            uint8_t* d = row + x * a.bpp;
            d[0] = v;
            if (a.bpp >= 3) {
                d[1] = v;
                d[2] = v;
            }
            if (a.bpp == 4)
                d[3] = 255;
        }
    }
    for (int r = 0; r < rows; ++r)
        std::memcpy(areaRow(a, r), next.data() + r * rowBytes, rowBytes);
    a.seen.swap(next);
}

// Keeps the stamp in one buffer.
//  - The producer is changing the area (differs from what we last saw): merge its bytes into
//    the background and wait until nothing new arrived for kSettle; writing earlier could
//    overwrite bytes it writes in the meantime.
//  - The area is stable: redraw if the producer had changed it, or if the value changed.
void Segment::refresh(Area& a, const digits::Stamp& stamp, bool valueChanged)
{
    if (!a.hasBg || a.seen.size() != size_t(digits::kWidth) * a.bpp * digits::kHeight) {
        captureBackground(a);
        draw(a, stamp);
        return;
    }
    const auto now = clock_type::now();
    if (mergeProducerWrites(a)) {
        a.pending = true;
        a.lastChange = now;
        return;
    }
    if (a.pending && now - a.lastChange < kSettle)
        return;
    if (!a.pending && !valueChanged)
        return;
    if (a.pending) {
        ++stats_.restores;
        a.hot = false; // the new frame has been copied over the stamp and is stamped again
    }
    a.pending = false;
    draw(a, stamp);
}

void Segment::tick(const digits::Stamp& stamp)
{
    stats_.problem.clear();
    RDB_SHM_HDR_t hdr{};
    if (!readAt(0, hdr) || hdr.headerSize == 0 || hdr.noBuffers == 0) {
        stats_.problem = "no RDB shared memory header";
        areas_.clear();
        return;
    }
    areas_.resize(hdr.noBuffers);
    for (size_t i = 0; i < areas_.size(); ++i) {
        updateArea(i, hdr.headerSize + i * sizeof(RDB_SHM_BUFFER_INFO_t));
        if (areas_[i].valid) {
            refresh(areas_[i], stamp, true);
            ++stats_.writes;
        }
    }
}

bool Segment::guard(const digits::Stamp& stamp)
{
    bool hot = false;
    for (auto& a : areas_) {
        if (!a.valid)
            continue;
        const uint32_t frame = frameNo(a);
        if (frame != a.lastFrame) {
            a.lastFrame = frame;
            a.hot = true;
            a.hotUntil = clock_type::now() + kHotWindow;
        }
        refresh(a, stamp, false);
        if (a.hot && clock_type::now() >= a.hotUntil)
            a.hot = false;
        hot = hot || a.hot || a.pending;
    }
    return hot;
}

void Segment::removeStamps()
{
    for (auto& a : areas_) {
        if (!a.valid || !a.hasBg || a.seen.size() != a.bg.size())
            continue;
        mergeProducerWrites(a); // anything VTD wrote meanwhile belongs to the background
        const size_t bytes = size_t(digits::kWidth) * a.bpp;
        for (int r = 0; r < digits::kHeight; ++r)
            std::memcpy(areaRow(a, r), a.bg.data() + r * bytes, bytes);
    }
}
