## Benchmark Results

| Operation | n | Median (ns) | p95 (ns) | p99 (ns) | Max (ns) | Throughput |
|-----------|---|-------------|----------|----------|----------|------------|
| Limit add (no match) | 20 | 482.1 | 539.6 | 551.3 | 554.3 | 2.1M ops/s |
| Limit add (full match) | 20 | 515.9 | 521.0 | 522.6 | 523.0 | 2.1M ops/s |
| Market (sweep 1) | 20 | 529.2 | 555.1 | 562.0 | 563.8 | 2.1M ops/s |
| Market (sweep 4) | 20 | 874.2 | 890.3 | 914.3 | 920.2 | 1.2M ops/s |
| Market (sweep 8) | 20 | 1294.3 | 1325.9 | 1331.9 | 1333.4 | 824K ops/s |
| Market (sweep 16) | 20 | 2248.7 | 2316.9 | 2336.2 | 2341.0 | 482K ops/s |
| Cancel | 20 | 3.7 | 3.7 | 3.8 | 3.8 | 291.4M ops/s |
| IOC | 20 | 535.7 | 546.0 | 546.0 | 546.0 | 2.0M ops/s |
| FOK (fillable) | 20 | 541.6 | 546.9 | 551.6 | 552.7 | 2.0M ops/s |
| FOK (cancelled) | 20 | 439.2 | 441.9 | 442.1 | 442.1 | 2.5M ops/s |
| best_bid /ask query | 20 | 2.5 | 2.7 | 2.8 | 2.8 | 455.6M ops/s |
| Snapshot (depth 5) | 20 | 202.8 | 209.7 | 210.6 | 210.9 | 5.1M ops/s |
| Snapshot (depth 10) | 20 | 295.1 | 303.7 | 303.9 | 304.0 | 3.7M ops/s |
| Snapshot (depth 20) | 20 | 444.4 | 462.8 | 464.1 | 464.4 | 2.5M ops/s |
| High contention | 20 | 125.8 | 126.7 | 126.7 | 126.7 | 8.6M ops/s |
| Mixed workload (1k resting orders) | 5 | 392.6 | 404.0 | 404.8 | 405.0 | 2.9M ops/s |
| Mixed workload (10k resting orders) | 5 | 393.1 | 406.1 | 407.3 | 407.5 | 2.8M ops/s |
| Mixed workload (100k resting orders) | 5 | 524.6 | 537.9 | 538.6 | 538.7 | 2.1M ops/s |
| Mixed workload (1000k resting orders) | 5 | 737.6 | 757.2 | 759.9 | 760.6 | 1.4M ops/s |
