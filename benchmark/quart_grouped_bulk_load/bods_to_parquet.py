#!/usr/bin/env python3
"""Convert a BoDS / QuART key file (raw little-endian integer keys) to Parquet with one column k.

Usage: bods_to_parquet.py <keys.bin> <out.parquet> [--dtype u32|u64] [--sample-density D] [--seed S]

u32 keys become UINTEGER (4-byte ART keys, as in QuART), u64 keys become UBIGINT.
--sample-density keeps each key with probability D (0 < D <= 1), turning a dense sorted set into a
sorted-with-gaps set over the same key range (the paper's density < 1 regime).
"""
import argparse
import numpy as np
import pyarrow as pa
import pyarrow.parquet as pq

parser = argparse.ArgumentParser()
parser.add_argument("src")
parser.add_argument("dst")
parser.add_argument("--dtype", choices=["u32", "u64"], default="u32")
parser.add_argument("--sample-density", type=float, default=1.0)
parser.add_argument("--seed", type=int, default=42)
args = parser.parse_args()

np_type, pa_type = (np.uint32, pa.uint32()) if args.dtype == "u32" else (np.uint64, pa.uint64())
keys = np.fromfile(args.src, dtype=np_type)
if args.sample_density < 1.0:
    rng = np.random.default_rng(args.seed)
    keys = keys[rng.random(keys.shape[0]) < args.sample_density]

table = pa.table({"k": pa.array(keys, type=pa_type)})
pq.write_table(table, args.dst, compression="zstd", row_group_size=1 << 20)
print(f"{args.dst}: {keys.shape[0]} keys")
