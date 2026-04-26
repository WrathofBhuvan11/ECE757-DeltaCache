/*
 * DeltaCacheCompression.hh
 *
 * Compression-opportunity estimation helpers for the DeltaCache project.
 * Supports Plain BDI, XOR+BDI, and Pairwise Delta+BDI.
 *
 * Phase 1 (stats-only): gem5 keeps full 64-byte DataBlocks unchanged.
 * These helpers only estimate how many bytes a line *would* occupy under
 * each scheme, recording the opportunity in stats counters.
 *
 * Algorithm reference: profile_compression.py (offline profiler).
 * Byte-wise modular arithmetic matches that script exactly.
 */

#ifndef __MEM_RUBY_COMMON_DELTACACHECOMPRESSION_HH__
#define __MEM_RUBY_COMMON_DELTACACHECOMPRESSION_HH__

#include <array>
#include <cstdint>
#include <string>

namespace gem5
{
namespace ruby
{

// -----------------------------------------------------------------------
// Algorithm selector (matches ruby_custom.py --delta-cache-compression=)
// -----------------------------------------------------------------------
enum class DeltaCacheCompressionAlgo
{
    None,
    PlainBDI,
    XorBDI,
    DeltaBDI
};

// Direction chosen when computing byte-wise modular delta
enum class DeltaDirection
{
    None,
    NewMinusBase,   // result[i] = (new[i] - base[i]) & 0xFF
    BaseMinusNew    // result[i] = (base[i] - new[i]) & 0xFF
};

// -----------------------------------------------------------------------
// Result returned from every compression query
// -----------------------------------------------------------------------
struct DeltaCacheCompressionResult
{
    DeltaCacheCompressionAlgo algo  = DeltaCacheCompressionAlgo::None;
    int  originalBytes              = 64;   // always 64
    int  storedBytes                = 64;   // estimated compressed size
    int  nonzeroBytes               = 64;   // nonzero bytes in stored form
    bool compressed                 = false;
    uint64_t baseAddr               = 0;    // address of chosen base line
    DeltaDirection direction        = DeltaDirection::None;
    // True iff XOR/Delta pairing actually beat the plain-BDI fallback for
    // this line (paired < solo). Always false for plainBDI() results and for
    // XorBDI/DeltaBDI calls that fell back to plain BDI on threshold miss
    // or because no candidate was available.
    bool pairWon                    = false;
};

// -----------------------------------------------------------------------
// Main helper class (all methods static, no state)
// -----------------------------------------------------------------------
class DeltaCacheCompression
{
  public:
    static constexpr int LineBytes = 64;
    using Line = std::array<uint8_t, LineBytes>;

    // ------------------------------------------------------------------
    // Primitive helpers
    // ------------------------------------------------------------------

    // Count bytes that are not zero
    static int nonzeroByteCount(const Line &line);

    // Byte-wise XOR of two lines
    static Line xorLine(const Line &a, const Line &b);

    // Byte-wise modular subtraction: result[i] = (minuend[i] - subtrahend[i]) & 0xFF
    // Matches profile_compression.py byte-wise delta semantics exactly.
    static Line deltaLine(const Line &minuend, const Line &subtrahend);

    // ------------------------------------------------------------------
    // BDI compressed-size estimator
    //
    // Tries all standard BDI encoding formats (in order of compressed size)
    // and returns the smallest that fits.  Uses the first element as base.
    // Returns 64 if nothing compresses.
    //
    // Supported formats (base_width x delta_width -> stored bytes):
    //   ZERO         : 0-byte base, 0-byte deltas ->  8 bytes (metadata)
    //   REP8         : 8-byte base, 0-byte deltas ->  8 bytes
    //   8x1          : 8-byte base, 1-byte deltas -> 16 bytes
    //   8x2          : 8-byte base, 2-byte deltas -> 24 bytes
    //   4x1          : 4-byte base, 1-byte deltas -> 20 bytes
    //   4x2          : 4-byte base, 2-byte deltas -> 36 bytes
    //   2x1          : 2-byte base, 1-byte deltas -> 34 bytes
    //   8x4          : 8-byte base, 4-byte deltas -> 40 bytes
    //   uncompressed :                             -> 64 bytes
    // ------------------------------------------------------------------
    static int bdiCompressedSize(const Line &line);

    // ------------------------------------------------------------------
    // Top-level per-mode compression estimators
    // ------------------------------------------------------------------

    // Plain BDI: estimate BDI size of a single line, no partner needed.
    static DeltaCacheCompressionResult plainBDI(const Line &line);

    // XOR + BDI: compute BDI(new XOR base); accept only if
    // nonzeroByteCount(xorResult) <= threshold.  Returns plainBDI(newLine)
    // as fallback so comparison against plain BDI is always fair.
    static DeltaCacheCompressionResult xorBDI(
        const Line    &newLine,
        const Line    &baseLine,
        uint64_t       baseAddr,
        int            threshold = 32);

    // Pairwise Delta + BDI: try both byte-wise modular delta directions,
    // choose the one with fewer nonzero bytes, then BDI compress it.
    // Accept only if nonzeroByteCount(bestDelta) <= threshold.
    // Falls back to plainBDI(newLine) if threshold not met.
    static DeltaCacheCompressionResult deltaBDI(
        const Line    &newLine,
        const Line    &baseLine,
        uint64_t       baseAddr,
        int            threshold = 32);

    // ------------------------------------------------------------------
    // Map-value hash for the N-to-1 Delta Cache map table.
    //
    // Implements the Sparse Byte Labeling (SBL) hash used in the XOR Cache
    // paper (Pan & San Miguel, ISCA'25, Section 5.1.3): for each 8-byte
    // word, take only the most significant 6 bytes, generate a 1-bit
    // sparse byte label per byte (1 if nonzero), then XOR-fold the
    // resulting 48 label bits into a `mapBits`-wide map value.
    //
    // Two value-similar lines will produce the same map value with high
    // probability, so the map table can find a good base candidate in O(1).
    // ------------------------------------------------------------------
    static uint32_t computeMapValue(const Line &line, int mapBits);

    // ------------------------------------------------------------------
    // Convert a gem5 DataBlock to a Line for algorithm use.
    // Caller must ensure DataBlock is allocated and block_size == LineBytes.
    // ------------------------------------------------------------------
    template<typename DataBlockT>
    static Line dataBlockToLine(const DataBlockT &db)
    {
        Line line;
        for (int i = 0; i < LineBytes; ++i)
            line[i] = db.getByte(i);
        return line;
    }

  private:
    // Internal BDI format checkers; return compressed size or INT_MAX if
    // the format does not fit.
    static int tryBDI_8x1(const Line &line);
    static int tryBDI_8x2(const Line &line);
    static int tryBDI_8x4(const Line &line);
    static int tryBDI_4x1(const Line &line);
    static int tryBDI_4x2(const Line &line);
    static int tryBDI_2x1(const Line &line);

    // Read a little-endian integer of width W bytes starting at byte offset i
    static int64_t  readSigned64(const Line &line, int i);
    static int64_t  readSigned32(const Line &line, int i);
    static int64_t  readSigned16(const Line &line, int i);
};

} // namespace ruby
} // namespace gem5

#endif // __MEM_RUBY_COMMON_DELTACACHECOMPRESSION_HH__
