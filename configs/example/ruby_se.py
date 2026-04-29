"""
ruby_se.py - generic SE-mode runner for the DeltaCache compression study.

Runs any static x86-64 ELF on:
  SimpleProcessor (X86, configurable CPU type)
  + MESITwoLevelCacheHierarchy (the MESI L2 we patched with
    recordDeltaCacheCompression)
  + SingleChannelDDR4_2400

The L2 is the LLC (no L3). Compression profiling stats land in stats.txt as
  board.cache_hierarchy.ruby_system.l2_controllers*.L2cache.dc_*

Usage
-----
$GEM5 -d <outdir> configs/example/ruby_se.py \\
    --binary <path/to/static-elf> \\
    --delta-cache-compression {None,PlainBDI,XorBDI,DeltaBDI} \\
    [--binary-args ...] \\
    [--num-cores N] [--cpu-type {timing,o3}] \\
    [--l2-size 1MiB --l2-assoc 16 --num-l2-banks 2]

This config does NOT require any base.cc patching - profiling is inline in
the Ruby L2 controller. No external trace, no offline analysis.
"""

import argparse

from gem5.coherence_protocol import CoherenceProtocol
from gem5.components.boards.simple_board import SimpleBoard
from gem5.components.cachehierarchies.ruby.deltacache_cache_hierarchy import (
    DeltaCacheCacheHierarchy,
)
from gem5.components.memory.single_channel import SingleChannelDDR4_2400
from gem5.components.processors.cpu_types import CPUTypes
from gem5.components.processors.simple_processor import SimpleProcessor
from gem5.isas import ISA
from gem5.resources.resource import BinaryResource
from gem5.simulate.simulator import Simulator
from gem5.utils.requires import requires

# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
parser = argparse.ArgumentParser(
    description="DeltaCache SE-mode runner - generic. Run any static "
    "x86-64 ELF and dump dc_* compression stats."
)

# Workload
parser.add_argument(
    "--binary",
    type=str,
    required=True,
    help="Path to the static x86-64 ELF to execute under SE mode.",
)
parser.add_argument(
    "--binary-args",
    nargs=argparse.REMAINDER,
    default=[],
    help="Optional argv passed to the binary (everything after this flag).",
)

# CPU / cores
parser.add_argument(
    "--cpu-type",
    type=str,
    default="timing",
    choices=["timing", "o3"],
    help="CPU model. 'timing' = TimingSimpleCPU (fast), "
    "'o3' = DerivO3CPU (more realistic, slower). Default: timing.",
)
parser.add_argument("--num-cores", type=int, default=1)
parser.add_argument("--clk-freq", type=str, default="3GHz")

# Compression mode (forwarded to L2 RubyCache)
parser.add_argument(
    "--delta-cache-compression",
    type=str,
    default="None",
    choices=["None", "PlainBDI", "XorBDI", "DeltaBDI"],
)
parser.add_argument("--delta-cache-xor-threshold", type=int, default=32)
parser.add_argument("--delta-cache-delta-threshold", type=int, default=32)

# Cache geometry
parser.add_argument("--l1i-size", type=str, default="32KiB")
parser.add_argument("--l1i-assoc", type=int, default=8)
parser.add_argument("--l1d-size", type=str, default="32KiB")
parser.add_argument("--l1d-assoc", type=int, default=8)
parser.add_argument("--l2-size", type=str, default="256KiB")
parser.add_argument("--l2-assoc", type=int, default=16)
parser.add_argument("--l3-size", type=str, default="2MiB")
parser.add_argument("--l3-assoc", type=int, default=16)
parser.add_argument(
    "--num-l3-banks",
    type=int,
    default=None,
    help="Number of LLC banks. If unset, defaults to the next power of two "
    ">= --num-cores so each core has a dedicated bank under contention.",
)
# kept for backward-compat with old run_params; effectively replaced by
# --num-l3-banks on the 3-level DeltaCache hierarchy (LLC is L3, not L2).
parser.add_argument("--num-l2-banks", type=int, default=2)

# Memory
parser.add_argument("--mem-size", type=str, default="2GiB")

args = parser.parse_args()

if args.num_l3_banks is None:
    n = max(1, args.num_cores)
    pow2 = 1
    while pow2 < n:
        pow2 *= 2
    args.num_l3_banks = pow2

# ---------------------------------------------------------------------------
# Sanity: this config is X86 + DeltaCache (matches build/X86_DeltaCache/gem5.opt).
# DeltaCache is a 3-level MESI clone with profiling hooks at the LLC (L3).
# ---------------------------------------------------------------------------
requires(
    isa_required=ISA.X86,
    coherence_protocol_required=CoherenceProtocol.DELTACACHE,
)

# ---------------------------------------------------------------------------
# 1. Cache hierarchy (DeltaCache 3-level: per-core L1+L2, shared L3 LLC)
# ---------------------------------------------------------------------------
cache_hierarchy = DeltaCacheCacheHierarchy(
    l1i_size=args.l1i_size,
    l1i_assoc=args.l1i_assoc,
    l1d_size=args.l1d_size,
    l1d_assoc=args.l1d_assoc,
    l2_size=args.l2_size,
    l2_assoc=args.l2_assoc,
    l3_size=args.l3_size,
    l3_assoc=args.l3_assoc,
    num_l3_banks=args.num_l3_banks,
    delta_cache_algo=args.delta_cache_compression,
    delta_cache_xor_threshold=args.delta_cache_xor_threshold,
    delta_cache_delta_threshold=args.delta_cache_delta_threshold,
)

# ---------------------------------------------------------------------------
# 2. Memory + processor
# ---------------------------------------------------------------------------
memory = SingleChannelDDR4_2400(size=args.mem_size)

cpu_type = CPUTypes.O3 if args.cpu_type == "o3" else CPUTypes.TIMING
processor = SimpleProcessor(
    cpu_type=cpu_type,
    isa=ISA.X86,
    num_cores=args.num_cores,
)

# ---------------------------------------------------------------------------
# 3. Board + workload
# ---------------------------------------------------------------------------
board = SimpleBoard(
    clk_freq=args.clk_freq,
    processor=processor,
    memory=memory,
    cache_hierarchy=cache_hierarchy,
)

binary = BinaryResource(local_path=args.binary)
board.set_se_binary_workload(binary=binary, arguments=args.binary_args)

print(f"[ruby_se] binary             : {args.binary}")
print(f"[ruby_se] binary args        : {args.binary_args}")
print(f"[ruby_se] cpu type           : {args.cpu_type}")
print(f"[ruby_se] num cores          : {args.num_cores}")
print(f"[ruby_se] compression algo   : {args.delta_cache_compression}")
print(f"[ruby_se] xor   threshold    : {args.delta_cache_xor_threshold}")
print(f"[ruby_se] delta threshold    : {args.delta_cache_delta_threshold}")
print(f"[ruby_se] L2 (private)       : {args.l2_size} / {args.l2_assoc}-way")
print(
    f"[ruby_se] L3 (LLC)           : {args.l3_size} / {args.l3_assoc}-way "
    f"/ {args.num_l3_banks} banks"
)

# ---------------------------------------------------------------------------
# 4. Run
# ---------------------------------------------------------------------------
simulator = Simulator(board=board)

print("Starting SE-mode Ruby simulation with DeltaCache profiling...")
simulator.run()
print("Simulation finished successfully")
print()
print(
    "Stats: grep '\\.l3_controllers[0-9]*\\.L2cache\\.dc_' "
    "<outdir>/stats.txt"
)
