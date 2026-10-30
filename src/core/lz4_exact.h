// QuattroStagioni - LZ4 block encoder that produces EXACTLY a requested compressed size.
//
// Crimson Desert stores LZ4 blocks (no frame) whose compressed size is recorded in several index
// files we must never modify (.pamt entry size, DDS header dwReserved1, .pathc). A replacement must
// therefore compress to exactly the original stored size.
//
// Method: compress with LZ4-HC (best ratio) to get a valid sequence list, then grow the block one
// step at a time with operations that keep it a valid LZ4 block:
//   * "shorten a match from its end": the last k bytes of a match become leading literals of the
//     next sequence (match start and offset unchanged, so all LZ4 end-of-block rules still hold);
//   * "drop a 4-byte minimum match": the whole match becomes literals merged with the neighbours.
// Each operation's size delta is computed exactly from the LZ4 length encodings, and the largest
// operation not exceeding the remaining gap is applied until the gap is zero. The result is then
// decoded with the reference LZ4_decompress_safe and must reproduce the input exactly while
// consuming the whole block.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace qs {

struct Lz4ExactResult {
    bool ok = false;
    std::vector<uint8_t> block;  // exactly targetSize bytes when ok
    size_t bestSize = 0;         // smallest size reached by LZ4-HC (for diagnostics)
    int hcLevel = 0;             // HC level whose parse was used
    std::string error;
};

// Encodes `src` as one LZ4 block of exactly `targetSize` bytes.
Lz4ExactResult lz4CompressExact(const uint8_t* src, size_t srcSize, size_t targetSize);

// Strict check: LZ4_decompress_safe must consume all `blockSize` bytes and yield exactly `expectedSize`
// bytes equal to `expected` (if non-null).
bool lz4VerifyStrict(const uint8_t* block, size_t blockSize, const uint8_t* expected, size_t expectedSize,
                     std::string* error = nullptr);

// Size of the LZ4 block that LZ4_compress_default would produce (for tools/tests).
size_t lz4DefaultSize(const uint8_t* src, size_t srcSize);

}  // namespace qs
