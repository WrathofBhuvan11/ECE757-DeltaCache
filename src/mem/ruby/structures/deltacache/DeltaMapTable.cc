#include "mem/ruby/structures/deltacache/DeltaMapTable.hh"

namespace gem5 {
namespace ruby {

DeltaMapTable::DeltaMapTable(const Params &p)
    : SimObject(p), m_block_size(p.block_size), m_table_entries(p.table_entries)
{
    m_direct_map_table.resize(m_table_entries, 0);
    m_valid_bits.resize(m_table_entries, false);
    inform("DeltaMapTable: initialized, %d entries, %d-byte lines\n",
           m_table_entries, m_block_size);
}

// Hash the DataBlock to a table index using the SBL (Significant Byte Label)
// approach: build a bitmask of which bytes are non-zero, then fold it down.
uint64_t
DeltaMapTable::generateMapValue(const DataBlock& blk)
{
    uint64_t byte_labels = 0;
    for (int i = 0; i < 64; ++i) {
        if (blk.getByte(i) != 0)
            byte_labels |= (1ULL << i);
    }
    byte_labels ^= (byte_labels >> 32);
    byte_labels ^= (byte_labels >> 16);
    return byte_labels;
}

int
DeltaMapTable::calculateCompressedSize(const DataBlock& blk)
{
    return m_block_size / 2;
}

void
DeltaMapTable::recordMapping(Addr addr, const DataBlock& blk)
{
    uint64_t map_val = generateMapValue(blk);
    uint32_t index = map_val % m_table_entries;

    if (m_valid_bits[index]) {
        Addr candidate_addr = m_direct_map_table[index];
        if (candidate_addr != addr) {
            inform("DeltaCache pair: index %d | new=0x%lx base=0x%lx\n",
                   index, addr, candidate_addr);
            m_valid_bits[index] = false;
        }
    } else {
        m_direct_map_table[index] = addr;
        m_valid_bits[index] = true;
    }
}

} // namespace ruby
} // namespace gem5
