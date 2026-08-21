#pragma once
// engine/package/package_format.hpp
// Native package format definitions - the compiler/runtime boundary
//
// This file defines the EMON package format that serves as the boundary
// between the Crystal frontend (compiler) and the generic runtime.
//
// The package format is owned by the engine, not the frontend.
// Frontends write packages; the runtime reads them.
//
// Package format:
// - Header (version, checksums, table of contents)
// - Map data (semantic structures, no ROM addresses)
// - Tileset atlases (pre-rendered RGBA, or native 8×8 tile+block format)
// - Sprite atlases
// - Audio data (converted from GB format)
// - Script bytecode (compiled Lua)

#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>

namespace enginemon {

//=============================================================================
// PACKAGE HEADER
//=============================================================================

struct PackageHeader {
    static constexpr uint32_t MAGIC = 0x454D4F4E;  // "EMON"
    static constexpr uint32_t VERSION = 3;  // v3: connection fields src_skip_blocks/strip_length_blocks/coord_adjust_tiles replace strip_offset/strip_length
    
    uint32_t magic;
    uint32_t version;
    uint32_t flags;
    
    // Source ROM info (for verification, not extraction)
    char source_sha1[41];       // Null-terminated hex string
    char source_version[32];    // e.g., "Crystal USA v1.1"
    
    // Table of contents offsets
    uint32_t toc_offset;
    uint32_t toc_size;
    
    // Checksums for integrity
    uint32_t data_crc32;
};

// =============================================================================
// WIRE-LAYOUT STATIC GUARANTEES
//
// PackageHeader is serialized as a raw struct (reinterpret_cast<char*>).
// These assertions pin the exact byte layout assumed by the writer and reader.
//
// If any assertion fires, the struct layout has changed (padding, field order,
// or compiler alignment rules differ from what was intended) and BOTH the
// writer and reader need updating.  Do not delete or relax these asserts —
// they exist precisely to catch accidental ABI changes.
//
// Expected layout on all current supported targets (Windows x64, MSVC/Clang):
//   offset  0 : magic          (uint32,  4 bytes)
//   offset  4 : version        (uint32,  4 bytes)
//   offset  8 : flags          (uint32,  4 bytes)
//   offset 12 : source_sha1[41](char[41],41 bytes)
//   offset 53 : source_version (char[32],32 bytes) — char[] has alignment 1, no padding
//   offset 85 : [3 bytes implicit padding to align toc_offset to 4]
//   offset 88 : toc_offset     (uint32,  4 bytes)
//   offset 92 : toc_size       (uint32,  4 bytes)
//   offset 96 : data_crc32     (uint32,  4 bytes)
//   total     : 100 bytes (multiple of 4 — no trailing padding needed)
//
// Endianness: the header is written/read as native bytes on the host.
// The current supported platforms are all little-endian x86-64.
// A future cross-platform port must replace the raw struct read/write with
// field-by-field little-endian serialization.
// =============================================================================

static_assert(std::is_standard_layout_v<PackageHeader>,
    "PackageHeader must be standard-layout for raw struct serialization");
static_assert(std::is_trivially_copyable_v<PackageHeader>,
    "PackageHeader must be trivially copyable for raw struct serialization");
static_assert(sizeof(PackageHeader) == 100,
    "PackageHeader wire size changed — update reader/writer or bump EMON_FORMAT_VERSION");
static_assert(offsetof(PackageHeader, magic)          ==  0,
    "PackageHeader::magic offset changed");
static_assert(offsetof(PackageHeader, version)        ==  4,
    "PackageHeader::version offset changed");
static_assert(offsetof(PackageHeader, flags)          ==  8,
    "PackageHeader::flags offset changed");
static_assert(offsetof(PackageHeader, source_sha1)    == 12,
    "PackageHeader::source_sha1 offset changed");
static_assert(offsetof(PackageHeader, source_version) == 53,
    "PackageHeader::source_version offset changed");
static_assert(offsetof(PackageHeader, toc_offset)     == 88,
    "PackageHeader::toc_offset offset changed — implicit padding changed?");
static_assert(offsetof(PackageHeader, toc_size)       == 92,
    "PackageHeader::toc_size offset changed");
static_assert(offsetof(PackageHeader, data_crc32)     == 96,
    "PackageHeader::data_crc32 offset changed");

//=============================================================================
// TABLE OF CONTENTS
//=============================================================================

enum class ChunkType : uint32_t {
    Maps = 0x4D415053,          // "MAPS"
    TilesetAtlases = 0x54494C53, // "TILS"
    Sprites = 0x53505254,        // "SPRT"
    ObjPalettes = 0x4F424A50,    // "OBJP"
    Scripts = 0x53435250,        // "SCRP"
    Audio = 0x41554449,          // "AUDI"
    Strings = 0x53545247,        // "STRG"
    Fonts = 0x464F4E54,          // "FONT"
};

struct TocEntry {
    ChunkType type;
    uint32_t offset;
    uint32_t size;
    uint32_t count;             // Number of items in chunk
    uint32_t crc32;             // Chunk checksum
};

//=============================================================================
// UTILITY
//=============================================================================

// Calculate CRC32 for data integrity
uint32_t calculate_crc32(const void* data, size_t size);

} // namespace enginemon
