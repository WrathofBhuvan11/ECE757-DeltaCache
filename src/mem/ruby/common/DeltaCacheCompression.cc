/*
 * DeltaCacheCompression.cc
 *
 * BDI algorithm semantics match profile_compression.py:
 *   - Little-endian word extraction
 *   - First element used as base
 *   - Signed delta fit check
 *   - Byte-wise modular arithmetic for XOR / Delta directions
 */

#include "mem/ruby/common/DeltaCacheCompression.hh"

#include <algorithm>
#include <climits>
#include <cstring>

namespace gem5
{
namespace ruby
{

// ----------------------------------------------------------
// Primitive helpers
// ----------------------------------------------------------

int
DeltaCacheCompression::nonzeroByteCount(const Line &line)
{
    int count = 0;
    for (uint8_t b : line)
        if (b != 0) ++count;
    return count;
}

DeltaCacheCompression::Line
DeltaCacheCompression::xorLine(const Line &a, const Line &b)
{
    Line result;
    for (int i = 0; i < LineBytes; ++i)
        result[i] = a[i] ^ b[i];
    return result;
}

DeltaCacheCompression::Line
DeltaCacheCompression::deltaLine(const Line &minuend, const Line &subtrahend)
{
    // Byte-wise modular subtraction, matches profile_compression.py:
    //   delta[i] = (minuend[i] - subtrahend[i]) & 0xFF
    Line result;
    for (int i = 0; i < LineBytes; ++i)
        result[i] = static_cast<uint8_t>(
            (static_cast<int>(minuend[i]) - static_cast<int>(subtrahend[i]))
            & 0xFF);
    return result;
}

// ----------------------------------------------------------
// Little-endian signed readers
// ----------------------------------------------------------

int64_t
DeltaCacheCompression::readSigned64(const Line &line, int i)
{
    uint64_t v = 0;
    for (int k = 0; k < 8; ++k)
        v |= static_cast<uint64_t>(line[i + k]) << (8 * k);
    return static_cast<int64_t>(v);
}

int64_t
DeltaCacheCompression::readSigned32(const Line &line, int i)
{
    uint32_t v = 0;
    for (int k = 0; k < 4; ++k)
        v |= static_cast<uint32_t>(line[i + k]) << (8 * k);
    return static_cast<int32_t>(v);   // sign-extend through int32_t
}

int64_t
DeltaCacheCompression::readSigned16(const Line &line, int i)
{
    uint16_t v = static_cast<uint16_t>(line[i]) |
                 (static_cast<uint16_t>(line[i + 1]) << 8);
    return static_cast<int16_t>(v);   // sign-extend through int16_t
}

// ----------------------------------------------------------
// BDI format checkers
// Each returns the compressed byte count or INT_MAX if the format fails.
// Stored bytes = base_bytes + num_elements * delta_bytes  (no metadata).
// ----------------------------------------------------------

// 8-byte base, 8 elements, 1-byte signed deltas -> 8 + 8 = 16 bytes
int
DeltaCacheCompression::tryBDI_8x1(const Line &line)
{
    int64_t base = readSigned64(line, 0);
    for (int i = 0; i < LineBytes; i += 8) {
        int64_t delta = readSigned64(line, i) - base;
        if (delta < -128 || delta > 127) return INT_MAX;
    }
    return 8 + 8 * 1;   // 16
}

// 8-byte base, 8 elements, 2-byte signed deltas -> 8 + 16 = 24 bytes
int
DeltaCacheCompression::tryBDI_8x2(const Line &line)
{
    int64_t base = readSigned64(line, 0);
    for (int i = 0; i < LineBytes; i += 8) {
        int64_t delta = readSigned64(line, i) - base;
        if (delta < -32768 || delta > 32767) return INT_MAX;
    }
    return 8 + 8 * 2;   // 24
}

// 8-byte base, 8 elements, 4-byte signed deltas -> 8 + 32 = 40 bytes
int
DeltaCacheCompression::tryBDI_8x4(const Line &line)
{
    int64_t base = readSigned64(line, 0);
    for (int i = 0; i < LineBytes; i += 8) {
        int64_t delta = readSigned64(line, i) - base;
        if (delta < INT32_MIN || delta > INT32_MAX) return INT_MAX;
    }
    return 8 + 8 * 4;   // 40
}

// 4-byte base, 16 elements, 1-byte signed deltas -> 4 + 16 = 20 bytes
int
DeltaCacheCompression::tryBDI_4x1(const Line &line)
{
    int64_t base = readSigned32(line, 0);
    for (int i = 0; i < LineBytes; i += 4) {
        int64_t delta = readSigned32(line, i) - base;
        if (delta < -128 || delta > 127) return INT_MAX;
    }
    return 4 + 16 * 1;  // 20
}

// 4-byte base, 16 elements, 2-byte signed deltas -> 4 + 32 = 36 bytes
int
DeltaCacheCompression::tryBDI_4x2(const Line &line)
{
    int64_t base = readSigned32(line, 0);
    for (int i = 0; i < LineBytes; i += 4) {
        int64_t delta = readSigned32(line, i) - base;
        if (delta < -32768 || delta > 32767) return INT_MAX;
    }
    return 4 + 16 * 2;  // 36
}

// 2-byte base, 32 elements, 1-byte signed deltas -> 2 + 32 = 34 bytes
int
DeltaCacheCompression::tryBDI_2x1(const Line &line)
{
    int64_t base = readSigned16(line, 0);
    for (int i = 0; i < LineBytes; i += 2) {
        int64_t delta = readSigned16(line, i) - base;
        if (delta < -128 || delta > 127) return INT_MAX;
    }
    return 2 + 32 * 1;  // 34
}

// ----------------------------------------------------------
// BDI top-level: try all formats, return smallest that fits
// ----------------------------------------------------------

int
DeltaCacheCompression::bdiCompressedSize(const Line &line)
{
    // Special case: all zeros -> store only metadata (~8 bytes)
    bool allZero = true;
    for (uint8_t b : line) { if (b) { allZero = false; break; } }
    if (allZero) return 8;

    // Special case: all 8-byte words equal -> 8 bytes
    int64_t first8 = readSigned64(line, 0);
    bool repVal = true;
    for (int i = 8; i < LineBytes; i += 8)
        if (readSigned64(line, i) != first8) { repVal = false; break; }
    if (repVal) return 8;

    // Try formats in order of ascending compressed size and return the first
    // that succeeds (they are already ordered smallest-first within their tier)
    int best = LineBytes;  // fallback: uncompressed

    int s;
    s = tryBDI_8x1(line); if (s < best) best = s;
    s = tryBDI_4x1(line); if (s < best) best = s;
    s = tryBDI_8x2(line); if (s < best) best = s;
    s = tryBDI_2x1(line); if (s < best) best = s;
    s = tryBDI_4x2(line); if (s < best) best = s;
    s = tryBDI_8x4(line); if (s < best) best = s;

    return best;
}

// ----------------------------------------------------------
// Top-level per-mode estimators
// ----------------------------------------------------------

DeltaCacheCompressionResult
DeltaCacheCompression::plainBDI(const Line &line)
{
    DeltaCacheCompressionResult res;
    res.algo          = DeltaCacheCompressionAlgo::PlainBDI;
    res.originalBytes = LineBytes;
    res.storedBytes   = bdiCompressedSize(line);
    res.nonzeroBytes  = nonzeroByteCount(line);
    res.compressed    = (res.storedBytes < LineBytes);
    return res;
}

DeltaCacheCompressionResult
DeltaCacheCompression::xorBDI(const Line &newLine,
                               const Line &baseLine,
                               uint64_t    baseAddr,
                               int         threshold)
{
    Line xored = xorLine(newLine, baseLine);
    int  nz    = nonzeroByteCount(xored);

    if (nz <= threshold) {
        int paired = bdiCompressedSize(xored);
        int solo   = bdiCompressedSize(newLine);
        int stored = std::min(paired, solo);   // fair vs plain BDI fallback

        DeltaCacheCompressionResult res;
        res.algo          = DeltaCacheCompressionAlgo::XorBDI;
        res.originalBytes = LineBytes;
        res.storedBytes   = stored;
        res.nonzeroBytes  = nz;
        res.compressed    = (stored < LineBytes);
        res.baseAddr      = baseAddr;
        res.direction     = DeltaDirection::NewMinusBase; // XOR is symmetric
        res.pairWon       = (paired < solo);   // true only if pairing helped
        return res;
    }

    // Threshold not met: fall back to plain BDI so the result is never
    // worse than plain BDI alone
    DeltaCacheCompressionResult res = plainBDI(newLine);
    res.algo    = DeltaCacheCompressionAlgo::XorBDI;
    res.pairWon = false;   // pairing did not contribute
    return res;
}

DeltaCacheCompressionResult
DeltaCacheCompression::deltaBDI(const Line &newLine,
                                 const Line &baseLine,
                                 uint64_t    baseAddr,
                                 int         threshold)
{
    // Try both byte-wise modular delta directions
    Line d1 = deltaLine(newLine, baseLine);   // new - base
    Line d2 = deltaLine(baseLine, newLine);   // base - new

    int nz1 = nonzeroByteCount(d1);
    int nz2 = nonzeroByteCount(d2);

    // Pick the direction with fewer nonzero bytes
    const Line     &bestDelta = (nz1 <= nz2) ? d1 : d2;
    DeltaDirection  dir       = (nz1 <= nz2)
                                    ? DeltaDirection::NewMinusBase
                                    : DeltaDirection::BaseMinusNew;
    int nzBest = std::min(nz1, nz2);

    if (nzBest <= threshold) {
        int paired = bdiCompressedSize(bestDelta);
        int solo   = bdiCompressedSize(newLine);
        int stored = std::min(paired, solo);   // fair vs plain BDI fallback

        DeltaCacheCompressionResult res;
        res.algo          = DeltaCacheCompressionAlgo::DeltaBDI;
        res.originalBytes = LineBytes;
        res.storedBytes   = stored;
        res.nonzeroBytes  = nzBest;
        res.compressed    = (stored < LineBytes);
        res.baseAddr      = baseAddr;
        res.direction     = dir;
        res.pairWon       = (paired < solo);   // true only if pairing helped
        return res;
    }

    // Threshold not met: fall back to plain BDI
    DeltaCacheCompressionResult res = plainBDI(newLine);
    res.algo    = DeltaCacheCompressionAlgo::DeltaBDI;
    res.pairWon = false;   // pairing did not contribute
    return res;
}

// ----------------------------------------------------------
// Map-value hash for the N-to-1 Delta Cache map table.
//
// Sparse Byte Labeling (SBL) hash from XOR Cache, ISCA'25 §5.1.3:
//   - View the 64-byte line as 8 little-endian 8-byte words.
//   - Per word, use only the most-significant 6 bytes (skip byte
//     offsets 0,1 within the word).
//   - Per used byte, emit a 1-bit "nonzero?" sparse label.
//   - Total: 8 * 6 = 48 sparse label bits.
//   - XOR-fold the 48 bits into a mapBits-wide value.
//
// Two value-similar lines collide with high probability, which makes
// the map table an O(1) base-candidate lookup.
// ----------------------------------------------------------
uint32_t
DeltaCacheCompression::computeMapValue(const Line &line, int mapBits)
{
    if (mapBits <= 0) return 0;

    // Step 1: build sparse byte labels into bits [0..47] of `labels`.
    uint64_t labels = 0;
    int bitPos = 0;
    for (int w = 0; w < LineBytes; w += 8) {
        // MSB 6 bytes of each little-endian word = offsets 2..7
        for (int b = 2; b < 8; ++b) {
            if (line[w + b] != 0)
                labels |= (uint64_t(1) << bitPos);
            ++bitPos;
        }
    }

    // Step 2: XOR-fold into mapBits-wide value.
    uint32_t mask = (mapBits >= 32) ? 0xFFFFFFFFu
                                    : ((uint32_t(1) << mapBits) - 1);
    uint32_t hash = 0;
    for (int shift = 0; shift < 48; shift += mapBits) {
        hash ^= static_cast<uint32_t>((labels >> shift) & mask);
    }
    return hash & mask;
}

} // namespace ruby
} // namespace gem5
