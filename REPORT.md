# Side-Channel Demonstrations on Apple Silicon

**Date:** 2026-10-20
**Host:** Mac16,6, Apple M4 Max, 36 GB, macOS 15.7.3 (24G419)
**Scope:** Five side-channel classes exercised with real, running code on this machine: cache contention, shared last-level cache (SLC) occupancy, SMC power telemetry, Spectre v1, and a data memory-dependent prefetcher (DMP) attempt.

> **Hardware note:** the machine was initially described as an "M5 Max". On-host identification reports **Apple M4 Max** (`sysctl -n machdep.cpu.brand_string`). Every result below is measured on the M4 Max; no M5 hardware was available. The M4-specific findings (cache geometry, TAGE behavior, DIT) should not be assumed to transfer unchanged to M5.

## Executive summary

| # | Class | Type | Result | Artifact |
|---|-------|------|--------|----------|
| 1 | L1/L2 cache contention | Covert channel | **16/16 bytes**, ~3.2 s | `poc1_cache_contention.c` |
| 2 | SLC occupancy | Covert channel | **80/80 bits**, payload `M4MAX-SLC` | `poc2_slc_occupancy.c` |
| 3 | SMC power telemetry | Covert channel | **20/20 bits**, payload `M4` | `poc3_smc_power.c` |
| 4 | Spectre v1 | Transient execution | **Positive**: 8/8 bytes `M4SECRET`; benchmark 73.80 % of bits | `spectre_m4/` |
| 5 | DMP | Hardware prefetcher | **Negative**: no usable signal (DIT-mitigated) | — |

All demonstrations run as an unprivileged user process. No kernel extension, root access, or debugging entitlement is used.

## 1. Cache contention covert channel

Two processes share a page of cache lines. The sender modulates memory pressure (bursts of loads) on selected lines; the receiver times loads to the same lines and decodes bits. L1/L2 contention is observable across processes on the same P-core cluster.

- Payload: 16 bytes, decoded **16/16**.
- Runtime: ~3.2 s.
- Source: `poc1_cache_contention.c` (uses `common.h` timing helpers).

## 2. SLC occupancy covert channel

The sender allocates and sweeps a large working set (512 MB victim buffer, 32 MB aggressor window) to modulate occupancy of the shared last-level cache (SLC); the receiver infers occupancy through access timing. A short preamble frame synchronizes the receiver.

- Payload: `M4MAX-SLC`, decoded **80/80 bits**.
- Source: `poc2_slc_occupancy.c`.

## 3. SMC power covert channel

The sender toggles CPU load to modulate package power. The receiver samples total package power through IOKit `AppleSMC` (key `PSTR`, little-endian float, roughly 1 Hz) and decodes bits using 4 s slots.

- Payload: `M4`, decoded **20/20 bits**.
- Observed levels: idle ≈ 20.25 W, busy ≈ 37.95 W; decision threshold ≈ 29.10 W.
- Source: `poc3_smc_power.c` (link with `-framework IOKit`).

## 4. Spectre v1 (Branch Different)

`spectre_m4/` is a modified copy of [Branch Different](https://github.com/cispa/BranchDifferent) (CISPA, DIMVA'22), which implements Spectre v1/v2 on Apple silicon. It is retained under its original MIT license (see `spectre_m4/LICENSE`; the eviction-set code carries a separate notice in `spectre_m4/evsets.LICENSE`).

### Results

- **Live demo:** `./leak.sh` recovers the 8-byte secret **`M4SECRET`**, 8/8 bytes, in ~15 s per invocation. Verified 4/4 independent invocations.
- **Original benchmark mode:** 60,457 / 81,920 correct bits (**73.80 %**), leaking 10,240 bytes in 119,079 ms; an earlier run measured 74.88 %. At 73.8 % of eight bit-attempts per byte, roughly six bits per byte are recovered — effectively a reliable nibble plus partial leakage of the second nibble.
- **Reliability:** a single run succeeds roughly 60–70 % of the time; five runs with per-byte majority voting make the demo stable.

### Why the upstream PoC does not work as-is on M4

1. **Cache geometry.** The L1D is 128 KB with 128 B lines and 16 KB pages. Because page bases are 16 KB-aligned, the L1 set index depends only on the page offset: `set = (addr >> 7) & 127`. Eviction sets therefore behave differently than on M1, and the upstream auto-calibrated eviction threshold (about 370 counter-thread units) sits above both L1 (~130) and L2 (~170) and far below DRAM (~500+).
2. **Hardware prefetcher.** Probing the 16 entries of `array2` sequentially at a 512 B stride lets the prefetcher warm entries 2–15, producing a broad "hit" plateau instead of a single signal. Fixed in `spectre.c` by probing in a random order with no adjacent indices and selecting the single fastest entry (excluding entry 0) instead of thresholding every entry.
3. **TAGE training.** The M4 branch predictor learns the fixed 9-training/1-attack pattern over repeated iterations. Randomized training lengths were tried but made results worse in this build (partly because the attack call was not last in the loop); the upstream fixed pattern was kept.
4. **Eviction-set side effects.** Failed eviction-set searches still register full 1,280-element lists (~20 MB traversal), which evict both L1 and L2 and actually enable the leak. "Successful" reduced sets evict only L1 and give a weaker signal. Forcing a full L1+L2 flush with `cache_remove_prepare_full` was counterproductive because it also evicted the secret's line.
5. **Zero nibbles are unreadable.** Entry 0 is excluded (the training value maps to it), so any byte with a zero nibble is lost. The earlier secret `M4SPECTR` leaked as `M4S^ECTR` (7/8) because `0x50` contains a zero nibble; changing the secret to `M4SECRET` removed the issue.
6. **Compiler folding (debug builds only).** At `-O2`, clang folds `*array_size` into a constant comparison, eliminating the cold out-of-bounds window. This affected the standalone debug harness (`dbg24.c`), not Branch Different, which builds at `-O0` by default. The fix used in the harness is a volatile load: `*(volatile size_t *)array_size`.

### Changes relative to upstream

- `spectre/config.h`: `ITERATIONS` 128, `BENCHMARK` 0, `VERBOSITY` 0.
- `spectre/main.c`: public string of 1,023 `A`s; secret `M4SECRET`; result printed as lowercase hex so majority voting can operate on bytes.
- `spectre/spectre.c`: randomized non-adjacent probe order plus fastest-entry (argmin) vote in the measurement loop; entry 0 excluded.
- `leak.sh`: runs the PoC five times, extracts the hex result line, majority-votes each byte, decodes Latin-1.

### Reproduction

```sh
cd spectre_m4
make -C spectre m1      # clang -I. -I../common -lpthread -pthread -lm ../common/*.c *.c -o spectre
./leak.sh               # prints: M4SECRET
```

Benchmark mode (one nibble per byte, ~2 minutes):

```sh
# set BENCHMARK 1 in spectre/config.h, rebuild, then:
./spectre/spectre
```

## 5. DMP: negative result

A data memory-dependent prefetcher probe was attempted. No usable address-dependent signal was recovered on this machine; the result is consistent with DIT (Data Independent Timing) being enabled and with the prefetcher not exposing the required dependency. This is recorded as a negative finding, not as evidence that the class is impossible on other configurations.

## Cross-cutting observations

- **Timer.** `mach_absolute_time` has a 41.67 ns quantum; CNTVCT is per-cluster and unusable across processes.
- **Cache maintenance.** `dc civac` is trapped/neutered at EL0; eviction is done with memory sweeps instead.
- **Page size.** 16 KB pages make L1 indexing offset-only, which is central to eviction-set construction on this SoC.
- **Isolation.** All channels are same-user, same-machine; they do not cross VM or process isolation boundaries by themselves.

## Limitations

- Results are from one machine and one OS build; run-to-run variance is significant for Spectre.
- The Spectre demo leaks a fixed local secret with a cooperative victim function; it is a proof of concept, not an end-to-end remote attack.
- The zero-nibble limitation is a property of this Branch Different configuration, not of Spectre v1 in general.

## Attribution and licensing

This is **not** copied code presented as original. `spectre_m4/` is a derivative of Branch Different:

- Upstream: https://github.com/cispa/BranchDifferent, commit `9a622451cb596ecd0f036f4ad931c777120c16f6` ("Updated Readme.md").
- License: MIT, Copyright (c) 2022 CISPA. The original `LICENSE` and `evsets.LICENSE` are kept verbatim in `spectre_m4/`.
- The upstream `Readme.md`, `benchmark/`, `common/`, and `covert_channel/` directories are included unmodified for context.
- Our modifications are limited to the four files listed in "Changes relative to upstream" above and are documented here.
- Demos 1–3 and the DMP investigation are original code written for this project.

MIT permits reuse, modification, and redistribution provided the copyright and permission notice are retained, which is the case here.
