#ifndef __MEM_RUBY_STRUCTURES_DELTA_MAP_TABLE_HH__
#define __MEM_RUBY_STRUCTURES_DELTA_MAP_TABLE_HH__

#include "params/DeltaMapTable.hh"
#include "sim/sim_object.hh"
#include "mem/ruby/common/Address.hh"
#include "mem/ruby/common/DataBlock.hh"

namespace gem5 {
namespace ruby {

class DeltaMapTable : public SimObject {
  public:
    typedef DeltaMapTableParams Params;
    DeltaMapTable(const Params &p);

    int calculateCompressedSize(const DataBlock& blk);
    void recordMapping(Addr addr, const DataBlock& blk);

  private:
    int m_block_size;
    int m_table_entries;
    std::vector<Addr> m_direct_map_table;
    std::vector<bool> m_valid_bits;
    uint64_t generateMapValue(const DataBlock& blk);
};

/**
 * Global wrapper functions called from SLICC (L2Cache controller).
 * SLICC passes the table as a pointer; these bridge to member functions.
 */
inline int calculateCompressedSize(DeltaMapTable& table, const DataBlock& blk) {
    return table.calculateCompressedSize(blk);
}

inline void recordMapping(DeltaMapTable& table, Addr addr, const DataBlock& blk) {
    table.recordMapping(addr, blk);
}

} // namespace ruby
} // namespace gem5
#endif
