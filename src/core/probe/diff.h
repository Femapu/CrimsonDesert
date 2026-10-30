// QuattroStagioni - change detection for probe snapshots (portable).
//
// The runtime probes dump instance memory periodically; to keep the log readable only what
// changed is written, with typed interpretations. FieldStats accumulates per-dword statistics over
// a whole session so the probe can point at candidates such as "a float that sweeps 0..24" (time of
// day) or "a float that stays in 0..1 and moves with snowfall" without anyone reading hex dumps.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace qs::probe {

enum class FieldType : uint8_t { U8, I8, U16, I16, U32, I32, U64, I64, F32, F64, Ptr };
bool parseFieldType(const std::string& s, FieldType& out);
const char* toString(FieldType t);
size_t fieldSize(FieldType t);
double fieldAsDouble(FieldType t, const uint8_t* p);
std::string formatField(FieldType t, const uint8_t* p);
// Floats compare with an absolute epsilon (NaN == NaN), integers bitwise.
bool fieldChanged(FieldType t, const uint8_t* oldP, const uint8_t* newP, double epsilon);

struct ChangedRange {
    size_t offset = 0;
    size_t length = 0;
};
// Ranges of differing bytes; ranges separated by fewer than mergeGap equal bytes are merged.
std::vector<ChangedRange> diffBytes(const uint8_t* a, const uint8_t* b, size_t n, size_t mergeGap = 4);
// Rounds ranges out to 4-byte alignment (and merges overlaps), so dwords are logged whole.
std::vector<ChangedRange> alignRanges(const std::vector<ChangedRange>& in, size_t n, size_t align = 4);

std::string hexBytes(const uint8_t* p, size_t n);  // "0a 1b 2c"
// "+0x34: 00 00 c0 41 (f32 24, u32 1103101952)" for each aligned dword of the range.
std::string describeDwords(size_t baseOffset, const uint8_t* p, size_t n);

// Per-dword statistics over a sequence of snapshots of the same size.
class FieldStats {
public:
    void reset(size_t size);
    void add(const uint8_t* snapshot);
    size_t samples() const { return samples_; }

    struct Candidate {
        size_t offset = 0;
        uint32_t changes = 0;
        float minF = 0, maxF = 0, lastF = 0;
        uint32_t minU = 0, maxU = 0, lastU = 0;
        bool floatValid = false;  // every sample was a finite, reasonably small float
    };
    // Dwords that changed at least minChanges times and whose float values stayed in [lo, hi].
    std::vector<Candidate> floatsInRange(float lo, float hi, uint32_t minChanges = 1) const;
    // Dwords that changed at least minChanges times (integers or floats), most active first.
    std::vector<Candidate> mostActive(size_t maxResults, uint32_t minChanges = 1) const;

private:
    size_t size_ = 0, samples_ = 0;
    std::vector<uint8_t> last_;
    std::vector<Candidate> cands_;  // one per aligned dword
};

}  // namespace qs::probe
