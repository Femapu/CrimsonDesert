// QuattroStagioni - read-only parser for Crimson Desert 2.04 "NNNN/0.pamt" group indexes.
//
// Layout (all little endian), verified on patch 2.04.00:
//   u32 self_hash (= hashlittle(pamt[12:], 0xC5EDE))   u32 paz_count   u32 format tag (0x610E0232)
//   paz_count x { u32 index; u32 paz_hash; u32 paz_size }
//   u32 len + directory-name trie   u32 len + file-name trie
//   u32 n + n x 16B folder { u32 folder_hash; u32 dir_node (0xFFFFFFFF = root); u32 first_file; u32 file_count }
//   u32 n + n x 20B file   { u32 name_node; u32 paz_offset; u32 stored_size; u32 size;
//                            u16 paz_index; u8 flags; u8 ext_index }
//   u32 len + extension table ("\0.dds\0.xml\0...", index 0 = no extension)
// Trie node: LEB128 back-distance to the parent node start (0 = root) + u8 len + chars.
// flags: low nibble = compression (0 none, 1 per-mip DDS, 2 LZ4 block), high nibble = crypto (3 = ChaCha20).
//
// This module never modifies index data; it is only used to locate and cross-check entries.
#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace qs {

struct PamtEntry {
    std::string path;       // lower-case, '/' separated, e.g. "ui/texture/cd_loading_00.dds"
    uint32_t pazIndex = 0;
    uint32_t offset = 0;
    uint32_t storedSize = 0;
    uint32_t size = 0;
    uint8_t flags = 0;
    uint32_t record = 0;    // index into the file table
    unsigned compression() const { return flags & 0x0F; }
    unsigned crypto() const { return flags >> 4; }
};

struct PamtPaz {
    uint32_t index = 0;
    uint32_t hash = 0;
    uint32_t size = 0;
};

class Pamt {
public:
    // Parses a whole pamt file. Returns false (and sets error()) on any structural problem.
    bool parse(const uint8_t* data, size_t size);

    uint32_t storedSelfHash() const { return selfHash_; }
    uint32_t computedSelfHash() const { return computedHash_; }
    uint32_t formatTag() const { return formatTag_; }
    const std::vector<PamtPaz>& pazTable() const { return paz_; }
    size_t fileCount() const { return files_.size(); }
    const std::string& error() const { return error_; }

    // Looks up an entry by path (case-insensitive, '\\' or '/' accepted, leading '/' ignored).
    bool find(const std::string& path, PamtEntry& out) const;

    // Enumerates every entry (used by tools/tests). Builds full paths on the fly.
    std::vector<PamtEntry> entries() const;

private:
    struct FileRec {
        uint32_t nameNode, offset, stored, size;
        uint16_t pazIndex;
        uint8_t flags, ext;
    };
    struct Folder {
        uint32_t hash, dirNode, first, count;
    };
    bool buildTrie(const uint8_t* tab, size_t len, std::unordered_map<uint32_t, std::string>& out);
    PamtEntry makeEntry(const std::string& dir, uint32_t rec) const;

    uint32_t selfHash_ = 0, computedHash_ = 0, formatTag_ = 0;
    std::vector<PamtPaz> paz_;
    std::vector<Folder> folders_;
    std::vector<FileRec> files_;
    std::vector<std::string> exts_;
    std::unordered_map<uint32_t, std::string> dirNames_, fileNames_;
    std::string error_;
};

std::string normalizePath(const std::string& p);  // lower-case, forward slashes, no leading '/'

}  // namespace qs
