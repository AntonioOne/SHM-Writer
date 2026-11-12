// One RDB image shared memory segment and the digit stamp kept in each of its buffers.
#pragma once

#include <sys/types.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "digits.h"
#include "rdb_shm.h"

struct SegmentStats {
    uint64_t writes = 0;      // stamp writes in write cycles (one per buffer per cycle)
    uint64_t restores = 0;    // the producer overwrote the stamp area; background merged, stamp redrawn
    std::string problem;      // why nothing can be written (empty if fine)
    std::string image;        // e.g. "960x540 RGB8"
};

class Segment {
public:
    explicit Segment(key_t key);
    ~Segment();
    Segment(const Segment&) = delete;
    Segment& operator=(const Segment&) = delete;

    bool attach();
    key_t key() const { return key_; }
    int id() const { return id_; }
    const SegmentStats& stats() const { return stats_; }

    // Write cycle: draws `stamp` into every buffer that holds a supported image.
    void tick(const digits::Stamp& stamp);

    // Between cycles: redraws `stamp` in buffers whose stamp area the producer has overwritten.
    // Returns true while a buffer is "hot": a new frame number was published and the producer
    // is expected to copy the frame over the stamp shortly -> the caller should poll without
    // sleeping so the stamp is restored within microseconds.
    bool guard(const digits::Stamp& stamp);

private:
    // Stamp area (top-right corner of the displayed image) of one buffer.
    struct Area {
        bool valid = false;
        size_t dataOffset = 0; // offset of the pixel data in the segment
        unsigned width = 0;
        unsigned height = 0;
        unsigned bpp = 0;
        size_t stride = 0;
        bool hasBg = false;
        std::vector<uint8_t> bg;   // producer pixels under the stamp
        std::vector<uint8_t> seen; // area content as last written or observed
        bool pending = false;      // producer changed the area; redraw once it is stable
        std::chrono::steady_clock::time_point lastChange; // last producer write seen in the area
        size_t msgOffset = 0;      // RDB message header of this buffer (frame number)
        uint32_t lastFrame = 0;
        bool hot = false;          // new frame published, stamp not yet restored
        std::chrono::steady_clock::time_point hotUntil;
        std::chrono::steady_clock::time_point failingSince; // image lookup failing since (epoch = ok)
    };

    template <typename T>
    bool readAt(size_t offset, T& out) const;
    bool locateImage(const RDB_SHM_BUFFER_INFO_t& info, RDB_IMAGE_t& img, size_t& dataOffset) const;
    void updateArea(size_t index, size_t infoOffset);
    void lookupFailed(Area& a, const std::string& problem);
    uint8_t* areaRow(const Area& a, int row) const;
    uint32_t frameNo(const Area& a) const;
    void captureBackground(Area& a);
    bool mergeProducerWrites(Area& a);
    void draw(Area& a, const digits::Stamp& stamp);
    void refresh(Area& a, const digits::Stamp& stamp, bool valueChanged);

    key_t key_;
    int id_ = -1;
    uint8_t* base_ = nullptr;
    size_t size_ = 0;
    std::vector<Area> areas_;
    SegmentStats stats_;
};
