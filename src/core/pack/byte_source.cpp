// QuattroStagioni - byte sources. See byte_source.h.
#include "byte_source.h"

#include <cstring>
#include <mutex>

#ifdef _WIN32
#include <filesystem>
#endif

namespace qs::pack {

FILE* fopenUtf8(const std::string& path, const char* mode) {
#ifdef _WIN32
    std::wstring wmode(mode, mode + std::strlen(mode));
    std::wstring wpath;
    try {
        wpath = std::filesystem::u8path(path).wstring();
    } catch (...) {
        return nullptr;
    }
    return _wfopen(wpath.c_str(), wmode.c_str());
#else
    return std::fopen(path.c_str(), mode);
#endif
}

bool seek64(FILE* f, uint64_t offset) {
#ifdef _WIN32
    return _fseeki64(f, (long long)offset, SEEK_SET) == 0;
#else
    return fseeko(f, off_t(offset), SEEK_SET) == 0;
#endif
}

static bool tellEnd(FILE* f, uint64_t& size) {
#ifdef _WIN32
    if (_fseeki64(f, 0, SEEK_END) != 0) return false;
    long long s = _ftelli64(f);
#else
    if (fseeko(f, 0, SEEK_END) != 0) return false;
    long long s = (long long)ftello(f);
#endif
    if (s < 0) return false;
    size = uint64_t(s);
    return true;
}

bool fileSize(const std::string& path, uint64_t& size) {
    FILE* f = fopenUtf8(path, "rb");
    if (!f) return false;
    bool ok = tellEnd(f, size);
    std::fclose(f);
    return ok;
}

bool readWholeFile(const std::string& path, std::vector<uint8_t>& out) {
    out.clear();
    FILE* f = fopenUtf8(path, "rb");
    if (!f) return false;
    uint64_t size = 0;
    bool ok = tellEnd(f, size) && size < (uint64_t(1) << 40) && seek64(f, 0);
    if (ok) {
        out.resize(size_t(size));
        ok = size == 0 || std::fread(out.data(), 1, out.size(), f) == out.size();
    }
    std::fclose(f);
    if (!ok) out.clear();
    return ok;
}

bool writeWholeFile(const std::string& path, const void* data, size_t n) {
    FILE* f = fopenUtf8(path, "wb");
    if (!f) return false;
    bool ok = n == 0 || std::fwrite(data, 1, n, f) == n;
    ok = std::fclose(f) == 0 && ok;
    return ok;
}

namespace {

class FileSource : public ByteSource {
public:
    FileSource(FILE* f, uint64_t size) : f_(f), size_(size) {}
    ~FileSource() override { std::fclose(f_); }
    bool readAt(uint64_t offset, void* dst, size_t n) override {
        if (n == 0) return true;
        if (offset > size_ || n > size_ - offset) return false;
        std::lock_guard<std::mutex> lock(m_);
        return seek64(f_, offset) && std::fread(dst, 1, n, f_) == n;
    }
    uint64_t size() const override { return size_; }

private:
    FILE* f_;
    uint64_t size_;
    std::mutex m_;
};

class MemorySource : public ByteSource {
public:
    explicit MemorySource(std::vector<uint8_t> d) : d_(std::move(d)) {}
    bool readAt(uint64_t offset, void* dst, size_t n) override {
        if (offset > d_.size() || n > d_.size() - offset) return false;
        if (n) std::memcpy(dst, d_.data() + offset, n);
        return true;
    }
    uint64_t size() const override { return d_.size(); }

private:
    std::vector<uint8_t> d_;
};

}  // namespace

std::unique_ptr<ByteSource> openFileSource(const std::string& path, std::string* error) {
    FILE* f = fopenUtf8(path, "rb");
    if (!f) {
        if (error) *error = "cannot open " + path;
        return nullptr;
    }
    uint64_t size = 0;
    if (!tellEnd(f, size)) {
        std::fclose(f);
        if (error) *error = "cannot size " + path;
        return nullptr;
    }
    return std::unique_ptr<ByteSource>(new FileSource(f, size));
}

std::unique_ptr<ByteSource> memorySource(std::vector<uint8_t> data) {
    return std::unique_ptr<ByteSource>(new MemorySource(std::move(data)));
}

}  // namespace qs::pack
