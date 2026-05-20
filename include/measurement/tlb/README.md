# TLB Benchmark

## Goal

This benchmark estimates the size of the data TLB.

It tries to find two limits:

- `L1 DTLB`
- `L2 TLB / STLB`

The benchmark does not read fixed CPU tables.
It measures real runtime behavior.

## Main Idea

The CPU reads data from more and more virtual pages.

- When all address translations fit in the TLB, access stays cheap.
- When the page count becomes too large, latency goes up.
- The first two stable jumps are used as estimates for `L1 DTLB` and `L2/STLB`.

The main metric is:

- `median cycles per access`

## Expected Result

The final output has two main values:

- `L1 DTLB estimate: N pages`
- `L2/STLB estimate: M pages`

The summary also shows coverage in bytes:

- `N * page_size`
- `M * page_size`

Example:

```text
L1 DTLB estimate: 64 pages (~262144 bytes coverage)
L2/STLB estimate: 512 pages (~2097152 bytes coverage)
```

## Algorithm

### 1. Build The Workload

The benchmark:

- builds a sweep like `1, 2, 4, 8, ... max_pages`
- allocates a page pool
- places one `PageNode` on each page
- spreads nodes across cache lines inside each page
- touches pages before timing, so page faults do not pollute the result
- warms the instruction path before the main sweep

Important helpers in [tlb_measurer.hpp](tlb_measurer.hpp):

- `build_page_counts()`
- `allocate_mapping()`
- `make_page_nodes()`
- `pretouch()`
- `warm_instruction_path()`

### 2. Measure One Point

For each `pages = N`, the benchmark:

1. shuffles the prepared page pool
2. takes the first `N` pages
3. links them into a ring
4. runs warm-up
5. runs the hot pointer-chasing loop
6. computes `cycles/access`
7. repeats the point several times

The hot loop uses dependent loads:

```cpp
cursor = cursor->next;
```

This is important because the next address depends on the previous load.
That reduces the effect of prefetching and memory-level parallelism.

### 3. Detect The Two Boundaries

After the sweep is complete, the benchmark:

1. smooths the `median cycles/access` curve
2. builds a baseline from the first points
3. finds the first stable jump for `L1 DTLB`
4. skips a small gap
5. finds the next stable jump for `L2/STLB`

Very small points are ignored for the `L1` candidate, so early noise does not become a false result.

Important helpers:

- `detect_boundaries()`
- `moving_average()`
- `find_jump()`

## User Config

The user-facing config is intentionally small:

```yaml
benchmarks:
  tlb:
    enabled: true
    measurement:
      max_pages: 4096
      iterations: 2000000
      huge_pages: false
```

Meaning:

- `max_pages`: largest sweep point
- `iterations`: number of dependent accesses in one timed sample
- `huge_pages`: use `2 MiB` huge pages instead of normal `4 KiB` pages

Config parsing is in:

- [src/app/config_loader.cpp](../../../src/app/config_loader.cpp)

## Summary Output

The final summary prints:

- `L1 DTLB estimate`
- `L2/STLB estimate`

Printing code:

- [include/core/summary_printer.hpp](../../../include/core/summary_printer.hpp)

## Short Validation Note

This text was checked against the current implementation:

- workload building in `TlbMeasurer`
- point measurement in `measure_point()` and `measure_cycles()`
- boundary detection in `detect_boundaries()`
- YAML config under `benchmarks.tlb`
