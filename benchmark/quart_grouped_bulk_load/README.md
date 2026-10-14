# Grouped (QuART) bulk loading: CREATE INDEX A/B benchmark

Times `CREATE INDEX` with grouped bulk loading on (`SET art_grouped_bulk_load = true`, the default)
and off (the per-chunk build-and-merge path) over the same tables.

## Workloads

`workloads.txt` lists `name|CREATE TABLE t AS ...` lines. Synthetic workloads are generated in SQL.
BoDS and QuART key files (raw little-endian integer keys) are converted to Parquet first:

```shell
python3 bods_to_parquet.py <keys.bin> WORKLOAD_DIR/bods20M_d100.parquet            # uint32 keys
python3 bods_to_parquet.py <keys.bin> WORKLOAD_DIR/bods20M_d050.parquet --sample-density 0.5
python3 bods_to_parquet.py <vackeys.bin> WORKLOAD_DIR/vac_banded_100M.parquet --dtype u64
```

`--sample-density D` keeps each key with probability `D`, which turns a dense sorted key set into a
sorted-with-gaps set over the same range (density below 1). Replace `WORKLOAD_DIR` in `workloads.txt`
with the directory holding the Parquet files.

## Running

```shell
WORKLOADS=$(cat workloads.txt) bash bench_create_index.sh build/reldebug/duckdb results.csv 3 1 8
python3 summarize.py results.csv      # Markdown table of medians and speedups
```

Arguments: DuckDB binary, output CSV, repeats per cell, then the thread counts to test.

## Results

`results/` holds runs from the development machine (96 cores, 125 GB RAM, RelWithDebInfo build).
Each `CREATE INDEX` time includes DuckDB's sort and the post-build index verification, so the
speedups understate the gain inside the ART build itself.
