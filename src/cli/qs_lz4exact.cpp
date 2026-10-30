// QuattroStagioni offline tool: encode a raw buffer as an LZ4 block of an exact compressed size.
// Usage: qs_lz4exact <input.raw> <target_size> <output.lz4>
// Exit code 0 on success (output verified with LZ4_decompress_safe), 1 on failure.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>

#include "../core/lz4_exact.h"

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: %s <input.raw> <target_size> <output.lz4>\n", argv[0]);
        return 2;
    }
    std::ifstream in(argv[1], std::ios::binary);
    std::vector<uint8_t> src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (src.empty()) { std::fprintf(stderr, "cannot read %s\n", argv[1]); return 1; }
    size_t target = std::strtoull(argv[2], nullptr, 10);
    qs::Lz4ExactResult r = qs::lz4CompressExact(src.data(), src.size(), target);
    std::printf("input %zu bytes, LZ4 default %zu, best HC %zu, target %zu -> %s%s (hc level %d)\n", src.size(),
                qs::lz4DefaultSize(src.data(), src.size()), r.bestSize, target, r.ok ? "OK" : "FAILED: ",
                r.ok ? "" : r.error.c_str(), r.hcLevel);
    if (!r.ok) return 1;
    std::ofstream out(argv[3], std::ios::binary);
    out.write(reinterpret_cast<const char*>(r.block.data()), std::streamsize(r.block.size()));
    return out.good() ? 0 : 1;
}
