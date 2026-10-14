#!/bin/bash
# Time CREATE INDEX with grouped bulk loading on and off.
#
# Usage: bench_create_index.sh <duckdb-binary> <out.csv> <repeat> <threads...>
# Workloads are read from the WORKLOADS variable: lines of "<name>|<CREATE TABLE ... AS ... SQL>".
# Each run loads the table once, then builds the index REPEAT times per (grouped, threads) cell.
# CSV columns: workload,grouped,threads,repeat,seconds
set -u
DUCKDB=$1; OUT=$2; REPEAT=$3; shift 3; THREADS="$@"
T=$(dirname "$OUT")

if [[ ! -f "$OUT" ]]; then
	echo "workload,grouped,threads,repeat,seconds" > "$OUT"
fi

while IFS='|' read -r name create_sql; do
	[[ -z "$name" ]] && continue
	for th in $THREADS; do
		for grouped in true false; do
			sql="$T/run_${name}_${th}_${grouped}.sql"
			{
				echo "SET threads=$th;"
				echo "SET art_grouped_bulk_load=$grouped;"
				echo "$create_sql"
				echo ".timer on"
				for ((r = 0; r < REPEAT; r++)); do
					echo "CREATE INDEX idx ON t(k);"
					echo ".timer off"
					echo "DROP INDEX idx;"
					echo ".timer on"
				done
			} > "$sql"
			"$DUCKDB" < "$sql" 2>&1 | grep -i "run time" | awk -v w="$name" -v g="$grouped" -v t="$th" \
				'{r++; print w "," g "," t "," r "," $5}' >> "$OUT"
		done
	done
done <<< "$WORKLOADS"
