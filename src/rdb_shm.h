// Minimal subset of VTD's viRDBIcd.h needed to locate RDB images in shared memory.
// Layouts must match the original (#pragma pack 4).
#pragma once

#include <cstdint>

#define RDB_MAGIC_NO 35712

#define RDB_PKG_ID_IMAGE 22

#define RDB_PIX_FORMAT_RGB_24      2
#define RDB_PIX_FORMAT_RGBA_24     5
#define RDB_PIX_FORMAT_BW_8        6
#define RDB_PIX_FORMAT_RED8       28
#define RDB_PIX_FORMAT_RGB8       39
#define RDB_PIX_FORMAT_RGBA8      40

#pragma pack(push, 4)

typedef struct {
    uint32_t headerSize;
    uint32_t dataSize;
    uint8_t  noBuffers;
} RDB_SHM_HDR_t;

typedef struct {
    uint32_t thisSize;
    uint32_t bufferSize;
    uint16_t id;
    uint16_t spare0;
    uint32_t flags;
    uint32_t offset;
    uint32_t spare1[4];
} RDB_SHM_BUFFER_INFO_t;

typedef struct {
    uint16_t magicNo;
    uint16_t version;
    uint32_t headerSize;
    uint32_t dataSize;
    uint32_t frameNo;
    double   simTime;
} RDB_MSG_HDR_t;

typedef struct {
    uint32_t headerSize;
    uint32_t dataSize;
    uint32_t elementSize;
    uint16_t pkgId;
    uint16_t flags;
} RDB_MSG_ENTRY_HDR_t;

typedef struct {
    uint32_t id;
    uint16_t width;
    uint16_t height;
    uint8_t  pixelSize;
    uint8_t  pixelFormat;
    uint16_t cameraId;
    uint32_t imgSize;
    uint8_t  color[4];
    uint32_t spare1[3];
} RDB_IMAGE_t;

#pragma pack(pop)

static_assert(sizeof(RDB_SHM_HDR_t) == 12, "RDB_SHM_HDR_t layout");
static_assert(sizeof(RDB_SHM_BUFFER_INFO_t) == 36, "RDB_SHM_BUFFER_INFO_t layout");
static_assert(sizeof(RDB_MSG_HDR_t) == 24, "RDB_MSG_HDR_t layout");
static_assert(sizeof(RDB_MSG_ENTRY_HDR_t) == 16, "RDB_MSG_ENTRY_HDR_t layout");
static_assert(sizeof(RDB_IMAGE_t) == 32, "RDB_IMAGE_t layout");
