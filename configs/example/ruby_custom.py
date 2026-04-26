import argparse
import m5

from gem5.components.boards.test_board import TestBoard
from gem5.components.cachehierarchies.ruby.mesi_two_level_cache_hierarchy import (
    MESITwoLevelCacheHierarchy,
)
from gem5.components.memory.single_channel import SingleChannelDDR4_2400
from gem5.components.processors.random_generator import RandomGenerator
from gem5.simulate.simulator import Simulator

# ---------------------------------------------------------------------------
# CLI argument parsing
# ---------------------------------------------------------------------------
parser = argparse.ArgumentParser(
    description="DeltaCache compression profiling on MESI_Two_Level "
                "(reuses build/X86/gem5.opt)"
)

# Compression mode
parser.add_argument(
    "--delta-cache-compression",
    type=str,
    default="None",
    choices=["None", "PlainBDI", "XorBDI", "DeltaBDI"],
    help="Compression algorithm to profile (default: None)",
)

# Thresholds (only used by XorBDI and DeltaBDI)
parser.add_argument(
    "--delta-cache-xor-threshold",
    type=int,
    default=32,
    help="XOR+BDI: max nonzero bytes in XOR line to accept a candidate "
         "(default: 32)",
)
parser.add_argument(
    "--delta-cache-delta-threshold",
    type=int,
    default=32,
    help="Delta+BDI: max nonzero bytes in delta line to accept a candidate "
         "(default: 32)",
)

# Cache/traffic knobs
# NOTE: stdlib MESI_Two_Level is L1 + L2 (no L3). The L2 here is the LLC
# where compression profiling is hooked in. Knob names kept similar so the
# user-facing CLI is unchanged.
parser.add_argument("--l2-size",      type=str, default="512KiB")
parser.add_argument("--l2-assoc",     type=int, default=64)
parser.add_argument("--num-l2-banks", type=int, default=2)
parser.add_argument("--num-cores",    type=int, default=1)
parser.add_argument("--duration",     type=str, default="1ms")
parser.add_argument("--rate",         type=str, default="10GB/s")
parser.add_argument("--rd-perc",      type=int, default=50,
                    help="Percentage of read requests (default: 50)")

args = parser.parse_args()

print(f"[ruby_custom] compression algo  : {args.delta_cache_compression}")
print(f"[ruby_custom] xor  threshold    : {args.delta_cache_xor_threshold}")
print(f"[ruby_custom] delta threshold   : {args.delta_cache_delta_threshold}")
print(f"[ruby_custom] num cores         : {args.num_cores}")

# ---------------------------------------------------------------------------
# 1. Setup the Ruby Cache Hierarchy (MESI_Two_Level)
#    L1 (private) + L2 (shared, banked) — L2 is LLC where we record compression
# ---------------------------------------------------------------------------
cache_hierarchy = MESITwoLevelCacheHierarchy(
    l1i_size="32KiB",
    l1i_assoc=8,
    l1d_size="32KiB",
    l1d_assoc=8,
    l2_size=args.l2_size,
    l2_assoc=args.l2_assoc,
    num_l2_banks=args.num_l2_banks,
    # Compression config forwarded all the way to RubyCache (CacheMemory)
    delta_cache_algo=args.delta_cache_compression,
    delta_cache_xor_threshold=args.delta_cache_xor_threshold,
    delta_cache_delta_threshold=args.delta_cache_delta_threshold,
)

# ---------------------------------------------------------------------------
# 2. Setup the Main Memory
# ---------------------------------------------------------------------------
memory = SingleChannelDDR4_2400(size="1GiB")

# ---------------------------------------------------------------------------
# 3. Setup the Traffic Generator
# ---------------------------------------------------------------------------
generator = RandomGenerator(
    num_cores=args.num_cores,
    duration=args.duration,
    rate=args.rate,
    block_size=64,
    min_addr=0,
    max_addr=100000,
    rd_perc=args.rd_perc,
    data_limit=0,
)

# ---------------------------------------------------------------------------
# 4. Assemble the board
# ---------------------------------------------------------------------------
board = TestBoard(
    clk_freq="1GHz",
    generator=generator,
    memory=memory,
    cache_hierarchy=cache_hierarchy,
)

# ---------------------------------------------------------------------------
# 5. Run
# ---------------------------------------------------------------------------
simulator = Simulator(board=board)

print("Starting MESI Ruby simulation with DeltaCache compression profiling...")
simulator.run()
print("Simulation finished successfully")
print()
print("Check m5out/stats.txt for dc_profiledLines, dc_storedBytes,")
print("dc_compressionRatio and related DeltaCache stats.")
