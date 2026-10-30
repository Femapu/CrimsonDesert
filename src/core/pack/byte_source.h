// QuattroStagioni - random access byte sources shared by the pack reader, the archive reader and the
// plugin. Implementations must allow concurrent readAt() calls from several threads.
// Paths are UTF-8; on Windows they are converted to UTF-16 (_wfopen), so non-ASCII game folders work.
#pragma once
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace qs::pack {

class ByteSource {
public:
    virtual ~ByteSource() = default;
    // Reads exactly `n` bytes at `offset`. Returns false on any short read or error.
    virtual bool readAt(uint64_t offset, void* dst, size_t n) = 0;
    virtual uint64_t size() const = 0;
};

// stdio based file source (one FILE*, reads serialised by a mutex). Returns nullptr on failure.
std::unique_ptr<ByteSource> openFileSource(const std::string& utf8Path, std::string* error = nullptr);

// In-memory source (tests, small files).
std::unique_ptr<ByteSource> memorySource(std::vector<uint8_t> data);

// fopen with a UTF-8 path on every platform.
FILE* fopenUtf8(const std::string& utf8Path, const char* mode);
// 64-bit seek/tell helpers for FILE*.
bool seek64(FILE* f, uint64_t offset);
bool fileSize(const std::string& utf8Path, uint64_t& size);
bool readWholeFile(const std::string& utf8Path, std::vector<uint8_t>& out);
bool writeWholeFile(const std::string& utf8Path, const void* data, size_t n);

}  // namespace qs::pack
