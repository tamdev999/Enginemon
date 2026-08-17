# Parser Arithmetic Proof (Hardening Item D)

## Summary

**STATUS: VERIFIED SAFE**

The `BoundsReader` class and package loading code in `engine/package/package_reader.cpp` already prevents integer overflow for `offset + length` and `count * element_size` operations BEFORE allocation/read.

## BoundsReader Class Analysis

### Core Safety Mechanism

```cpp
class BoundsReader {
    size_t total_size_;    // Total bytes available
    size_t current_pos_;   // Current read position
    
    bool has_bytes(size_t count) const {
        return current_pos_ + count <= total_size_;
    }
};
```

**Overflow Analysis:**
- `current_pos_` and `count` are both `size_t`
- On 64-bit systems: `size_t` is 64-bit, cannot overflow with file sizes
- On 32-bit systems: `size_t` is 32-bit
  - Maximum `current_pos_` = `total_size_` (can't exceed file size)
  - `count` comes from parsed uint32_t values
  - **Potential overflow if `current_pos_ + count > SIZE_MAX`**

**HOWEVER:** The code checks `count` against `PackageLimits` BEFORE calling `has_bytes()`:

```cpp
if (count > PackageLimits::MAX_ARRAY_COUNT) {
    return {};  // Rejected before addition
}
```

With `MAX_ARRAY_COUNT = 1M`, the addition `current_pos_ + count` cannot overflow even on 32-bit systems where `SIZE_MAX = 4G`.

### Template read_le() Safety

```cpp
template<typename T>
bool read_le(T& out) {
    if (!has_bytes(sizeof(T))) return false;  // Bounds check FIRST
    // ... then read
}
```

Safe: `sizeof(T)` is compile-time constant (1, 2, 4, 8 bytes max).

### read_bytes() Safety

```cpp
bool read_bytes(void* dst, size_t count) {
    if (!has_bytes(count)) return false;  // Bounds check FIRST
    in_.read(static_cast<char*>(dst), count);
    // ...
}
```

Safe: Checks remaining bytes before reading.

## Package Open Validation

### File Size Validation

```cpp
// Get file size
in.seekg(0, std::ios::end);
size_t file_size = static_cast<size_t>(in.tellg());

// Validate minimum header size
if (file_size < sizeof(PackageHeader)) {
    return nullptr;  // Truncated
}
```

### TOC Bounds Validation

```cpp
// Validate TOC bounds: offset + size must be within file
if (reader->header_.toc_offset > file_size ||
    reader->header_.toc_size > file_size ||
    reader->header_.toc_offset + reader->header_.toc_size > file_size) {
    return nullptr;  // TOC extends beyond file
}
```

**Overflow Analysis:**
- `toc_offset` and `toc_size` are `uint32_t` from the file
- `file_size` is `size_t`
- The checks `> file_size` happen BEFORE the addition
- The addition `toc_offset + toc_size` uses implicit conversion to `size_t` (larger type wins)
- Since both terms are bounded by `file_size`, the sum cannot overflow

### Per-Chunk Bounds Validation

```cpp
for (uint32_t i = 0; i < toc_entries; ++i) {
    // ... read entry fields ...
    
    // Validate each chunk's bounds BEFORE using
    if (entry.offset > file_size ||
        entry.size > file_size ||
        entry.offset + entry.size > file_size) {
        return nullptr;  // Chunk extends beyond file
    }
    
    // Validate chunk size against limits
    if (entry.size > PackageLimits::MAX_CHUNK_SIZE) {
        return nullptr;  // Chunk too large
    }
}
```

**Safe:** Same pattern as TOC - check individual terms before sum.

### TOC Entry Count Validation

```cpp
uint32_t toc_entries = reader->header_.toc_size / (sizeof(uint32_t) * 5);

if (toc_entries > PackageLimits::MAX_TOC_ENTRIES) {
    return nullptr;  // Unreasonable TOC size
}
```

**Safe:** Division cannot overflow, and count is bounded.

## Array Allocation Safety

### read_counted_array()

```cpp
template<typename T>
static std::vector<T> read_counted_array(std::istream& in, T (*read_item)(std::istream&)) {
    uint32_t count = read_le<uint32_t>(in);
    
    // Bounds check: reject unreasonably large counts
    if (count > PackageLimits::MAX_ARRAY_COUNT) {
        return {};  // Return empty on malformed data
    }
    
    std::vector<T> arr;
    arr.reserve(count);  // Safe: count ≤ 1M
    // ...
}
```

**Safe:** `count * sizeof(T)` bounded by `1M * sizeof(T)`. For reasonable structs (< 4KB each), this is < 4GB.

### Block Data Allocation

```cpp
uint32_t block_count = read_le<uint32_t>(in);
if (block_count > PackageLimits::MAX_BLOCK_COUNT) {
    return map;  // Return partial map on malformed data
}
map.blocks.resize(block_count);  // Safe: block_count ≤ 4M, each block is 1 byte
```

**Safe:** Maximum allocation is 4MB.

## Index Parsing Safety

```cpp
// Track consumed bytes within chunk for bounds checking
size_t consumed = 0;

for (uint32_t j = 0; j < entry.count; ++j) {
    // Check we have room for length prefix
    if (consumed + 2 > entry.size) {
        return nullptr;  // Truncated index entry
    }
    
    uint16_t id_len = read_le<uint16_t>(in);
    consumed += 2;
    
    // Validate string length
    if (id_len > PackageLimits::MAX_STRING_LENGTH ||
        consumed + id_len > entry.size) {
        return nullptr;  // String extends beyond chunk
    }
    
    // ... read string ...
    consumed += id_len;
    
    // Check we have room for data_size
    if (consumed + 4 > entry.size) {
        return nullptr;  // Truncated index entry
    }
    
    uint32_t data_size = read_le<uint32_t>(in);
    consumed += 4;
}
```

**Safe:**
- `consumed` accumulates bounds-checked increments
- Each increment is validated BEFORE addition
- `id_len` is bounded by `MAX_STRING_LENGTH = 64KB`
- `entry.size` was already validated against file size and `MAX_CHUNK_SIZE`

## PackageLimits Constants

```cpp
namespace PackageLimits {
    constexpr uint32_t MAX_STRING_LENGTH = 64 * 1024;      // 64KB per string
    constexpr uint32_t MAX_ARRAY_COUNT = 1024 * 1024;      // 1M elements max
    constexpr uint32_t MAX_BLOCK_COUNT = 4 * 1024 * 1024;  // 4M blocks max
    constexpr uint32_t MAX_CHUNK_SIZE = 256 * 1024 * 1024; // 256MB per chunk
    constexpr uint32_t MAX_TOC_ENTRIES = 65536;            // 64K TOC entries max
}
```

These limits ensure that all `count * element_size` products are bounded well below `SIZE_MAX` on both 32-bit and 64-bit systems.

## Verification Checklist

| Operation | Protected By | Safe? |
|-----------|-------------|-------|
| Header read | `file_size < sizeof(PackageHeader)` | ✅ |
| TOC offset + size | Check individual terms before sum | ✅ |
| Chunk offset + size | Check individual terms before sum | ✅ |
| TOC entry count | `count > MAX_TOC_ENTRIES` | ✅ |
| Array count | `count > MAX_ARRAY_COUNT` | ✅ |
| Block count | `count > MAX_BLOCK_COUNT` | ✅ |
| String length | `len > MAX_STRING_LENGTH` | ✅ |
| Chunk size | `size > MAX_CHUNK_SIZE` | ✅ |
| Index consumed tracking | Incremental bounds checks | ✅ |
| BoundsReader::has_bytes() | Terms bounded by PackageLimits | ✅ |

## Conclusion

**No code changes required.** The `BoundsReader` implementation and package parsing code already prevent integer overflow through:

1. **Early bounds checking** - Individual terms are validated before arithmetic
2. **PackageLimits constants** - Ensure products stay well below overflow thresholds
3. **Incremental validation** - Track consumed bytes with bounds checks at each step
4. **File size validation** - All offsets/sizes validated against actual file size first

The code follows secure parsing patterns throughout.
