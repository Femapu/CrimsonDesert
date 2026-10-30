// QuattroStagioni - read-only access to the user's Crimson Desert archives, for the generator.
//
// Groups: <game>/NNNN/0.pamt (base game) and <game>/dlc/<name>/NNNN/0.pamt (DLC), named "0012" and
// "dlc/dlc_01/0015". An entry's stored bytes live in <group>/<pazIndex>.paz at [offset, +storedSize).
// Stored -> plain: ChaCha20 decrypt when crypto == 3 (key from the basename and the pamt key material),
// then decompress: 0 none, 1 per-mip DDS (dds.h), 2 one LZ4 block. Anything else is refused.
//
// Plain -> stored (encodeStoredLike): rebuilds the stored form in the ORIGINAL layout with exactly
// the original stored size. For per-mip DDS the header (including the four per-mip stored sizes that
// meta/0.pathc also records) is copied verbatim from the original, every LZ4 mip is encoded with
// lz4CompressExact to its original stored size and raw mips stay raw. The result is re-encrypted if
// the original was encrypted, then decoded again strictly and compared with the input.
// Nothing here writes to the game folder.
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "../dds.h"
#include "../pack/byte_source.h"
#include "../pamt.h"

namespace qs::gen {

struct Group {
    std::string name;                 // "0012", "dlc/dlc_01/0015"
    std::string dir;                  // full folder path (UTF-8)
    Pamt pamt;
    uint8_t keyMaterial = 0;          // ChaCha key material from the pamt format tag
    std::unordered_map<std::string, PamtEntry> byPath;
};

class GameArchive {
public:
    // Enumerates the groups of the game folder (does not parse pamts yet).
    bool open(const std::string& gameDir, std::string* error = nullptr);
    const std::string& root() const { return root_; }
    const std::vector<std::string>& groupNames() const { return names_; }
    bool hasGroup(const std::string& name) const;

    // Parses the group's pamt on first use (thread safe). Returns nullptr and sets error on failure.
    const Group* group(const std::string& name, std::string* error = nullptr);

    std::string pazPath(const std::string& group, uint32_t pazIndex) const;
    bool pazFileSize(const std::string& group, uint32_t pazIndex, uint64_t& size);

    // Reads the stored bytes of an entry (thread safe).
    bool readStored(const std::string& group, const PamtEntry& e, std::vector<uint8_t>& out, std::string* error = nullptr);

    // bin64/CrimsonDesert.exe size and PE TimeDateStamp.
    bool exeBuild(uint64_t& size, uint32_t& timestamp) const;

private:
    std::string root_;
    std::vector<std::string> names_;
    std::mutex m_;
    std::map<std::string, std::unique_ptr<Group>> groups_;
    std::map<std::string, std::unique_ptr<pack::ByteSource>> paz_;
};

// Decoded structure of one stored entry.
struct EntryLayout {
    unsigned compression = 0, crypto = 0;
    uint32_t storedSize = 0, size = 0;
    uint8_t keyMaterial = 0;
    std::string path;                       // for the ChaCha key
    // compression 1 only:
    DdsInfo dds;
    std::vector<uint8_t> header;            // stored header bytes (128 or 148)
    std::vector<uint32_t> mipStored;        // per mip stored size (raw size for raw mips)
    std::vector<uint64_t> mipStoredOffset;  // per mip offset inside the stored blob
    std::vector<uint64_t> mipPlainOffset;   // per mip offset inside the plain DDS
    bool mipIsLz4(size_t i) const { return mipStored[i] < dds.mipRawSize[i]; }
};

// Decrypts + decompresses `stored` strictly. Fills `layout` (if non-null) and `plain`.
bool decodeStored(const PamtEntry& e, uint8_t keyMaterial, const uint8_t* stored, size_t storedSize,
                  std::vector<uint8_t>& plain, EntryLayout* layout = nullptr, std::string* error = nullptr);

// Result of re-encoding one mip (per-mip DDS) into its stored chunk.
struct MipEncode {
    bool ok = false;
    std::vector<uint8_t> chunk;   // exactly layout.mipStored[mip] bytes when ok
    size_t bestSize = 0;          // smallest LZ4 size reached (diagnostics for the fitter)
    std::string error;
};
// Encodes plain mip bytes (raw size) into the stored chunk of mip `mip`.
MipEncode encodeMipStored(const EntryLayout& layout, size_t mip, const uint8_t* rawMip, size_t rawSize);

// Assembles the stored blob from per-mip chunks (compression 1), encrypts if needed, then decodes the
// result strictly and compares it with `expectedPlain`. `out` has exactly layout.storedSize bytes.
bool assembleStored(const EntryLayout& layout, const std::vector<std::vector<uint8_t>>& chunks,
                    const std::vector<uint8_t>& expectedPlain, std::vector<uint8_t>& out, std::string* error = nullptr);

// Whole-entry re-encode in the original layout (any compression). On a per-mip LZ4 that does not fit,
// returns false and sets `failedMip`. Verifies the result by decoding it again.
bool encodeStoredLike(const EntryLayout& layout, const std::vector<uint8_t>& plain, std::vector<uint8_t>& out,
                      std::string* error = nullptr, int* failedMip = nullptr);

// Reads size and PE TimeDateStamp of an executable.
bool readExeBuild(const std::string& exePath, uint64_t& size, uint32_t& timestamp);

}  // namespace qs::gen
