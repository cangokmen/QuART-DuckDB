| workload | 1 thr: grouped / old / speedup | 8 thr: grouped / old / speedup |
|---|---|---|
| sorted_int_100M | 5.07 s / 11.33 s / 2.24x | 2.35 s / 3.12 s / 1.33x |
| sorted_bigint_100M | 6.88 s / 12.55 s / 1.82x | 2.53 s / 3.34 s / 1.32x |
| sparse_bigint_100M | 17.05 s / 23.56 s / 1.38x | 10.56 s / 11.70 s / 1.11x |
| random_ubigint_100M | 26.75 s / 33.92 s / 1.27x | 17.90 s / 18.82 s / 1.05x |
| dup4_int_100M | 12.77 s / 14.40 s / 1.13x | 3.62 s / 3.84 s / 1.06x |
| varchar_seq_20M | 4.33 s / 5.81 s / 1.34x | 2.30 s / 2.53 s / 1.10x |
| varchar_md5_20M | 11.03 s / 13.13 s / 1.19x | 4.91 s / 5.16 s / 1.05x |
| bods20M_d100 | 1.11 s / 2.26 s / 2.04x | 0.51 s / 0.63 s / 1.25x |
| bods20M_d050 | 0.62 s / 1.22 s / 1.95x | 0.38 s / 0.45 s / 1.19x |
| bods20M_d025 | 0.34 s / 0.65 s / 1.89x | 0.28 s / 0.31 s / 1.11x |
| bods20M_K25L25 | 1.76 s / 2.85 s / 1.62x | 0.61 s / 0.74 s / 1.20x |
| vac_banded_100M | 0.02 s / 0.03 s / 1.89x | 0.02 s / 0.02 s / 1.38x |
| vac_range25_100M | 0.01 s / 0.02 s / 2.12x | 0.01 s / 0.02 s / 1.78x |
| vac_mixed_250M | 0.07 s / 0.13 s / 2.03x | 0.03 s / 0.05 s / 1.52x |
| bods200M_d100 | 12.28 s / 22.32 s / 1.82x | 5.17 s / 6.64 s / 1.28x |
| bods200M_d050 | 6.98 s / 13.33 s / 1.91x | 3.99 s / 4.68 s / 1.17x |
