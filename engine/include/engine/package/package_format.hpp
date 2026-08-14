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
// - Tileset atlases (pre-rendered RGBA)
// - Collision data
// - Sprite atlases
// - Audio data (converted from GB format)
// - Script bytecode (compiled Lua)

#include <cstdint>
#include <string>

namespace enginemon {

//=============================================================================
// PACKAGE HEADER
//=============================================================================

struct PackageHeader {
    static constexpr uint32_t MAGIC = 0x454D4F4E;  // "EMON"
    static constexpr uint32_t VERSION = 1;
    
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

//=============================================================================
// TABLE OF CONTENTS
//=============================================================================

enum class ChunkType : uint32_t {
    Maps = 0x4D415053,          // "MAPS"
    TilesetAtlases = 0x54494C53, // "TILS"
    Collision = 0x434F4C4C,      // "COLL"
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
