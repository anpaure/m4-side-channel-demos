# Apple M4 Max side-channel demonstrations

Five local side-channel experiments on an Apple M4 Max (36 GB, macOS 15.7.3). Four produced usable signals; one did not. Everything runs as an unprivileged user process, with no kernel extension, root access, or debugging entitlement.

**Scope:** this is a reproduction and engineering effort, not a claim of new attack research. No new attack primitive is claimed, and no "first on M4" claim is made. See [Novelty and prior art](#novelty-and-prior-art).

**Hardware note:** the machine was initially described as an M5 Max. On-host identification reports **Apple M4 Max** (`sysctl -n machdep.cpu.brand_string`). All results below are from the M4 Max; no M5 hardware was tested.

## Results at a glance

| # | Experiment | What it demonstrates | Result | Code |
|---|------------|----------------------|--------|------|
| 1 | L1/L2 cache contention | Two-process cache-contention covert channel | **16/16 bytes**, ~3.2 s | `demos/poc1_cache_contention.c` |
| 2 | SLC occupancy | Shared last-level cache occupancy covert channel | **80/80 bits**, payload `M4MAX-SLC` | `demos/poc2_slc_occupancy.c` |
| 3 | SMC power telemetry | Package-power covert channel via `AppleSMC` key `PSTR` | **20/20 bits**, payload `M4` | `demos/poc3_smc_power.c` |
| 4 | Spectre v1 | Transient out-of-bounds read via mistrained branch | **8/8 bytes** `M4SECRET`; benchmark 73.80 % of bits | `spectre_m4/` |
| 5 | DMP | Data memory-dependent prefetcher probe | **Negative**: no usable signal | — |

## What was done here

- **Wrote three standalone covert-channel PoCs from scratch** (demos 1–3): the shared-memory protocols, sender/receiver synchronization, timing calibration, and decoding are original to this project. `common.h` contains the shared timing and page utilities.
- **Wrote the DMP probe** and recorded the negative result.
- **Ported Branch Different to this M4 Max and made the Spectre v1 demo reliable.** The exploit core is upstream code; the port and the attack-side fixes are the work described in [Spectre v1](#4-spectre-v1-branch-different).
- **Diagnosed the M4-specific failure modes** of the upstream Spectre PoC: hardware prefetcher interference, TAGE learning the fixed training pattern, eviction-set side effects, and the zero-nibble limitation.
- **Built the decoding harness**: `spectre_m4/leak.sh` runs the PoC five times and majority-votes each byte, turning a ~60–70 % per-run success rate into a stable demo.
- **Documented everything**, including what is original and what is adapted.

## How the demos work

### 1. L1/L2 cache contention

**Mechanism.** Two processes share a control block and a small set of cache lines. The sender modulates memory pressure on selected lines with bursts of loads; the receiver times loads to the same lines. A contended line is measurably slower, which gives one bit per slot.

**Method.** 128-byte lines, 256 candidate lines, 100 ms slots, 50,000-load bursts, two rounds, 4 ms guard interval, 16-byte payload.

**Run.**

```sh
clang -O2 -I. demos/poc1_cache_contention.c -o /tmp/poc1
/tmp/poc1
```

**Observed.** 16/16 bytes decoded in about 3.2 s.

### 2. SLC occupancy

**Mechanism.** The sender allocates and repeatedly sweeps a large buffer, changing occupancy of the shared last-level cache (SLC). The receiver infers occupancy from access timing to its own buffer. An alternating preamble frame synchronizes the two sides.

**Method.** 512 MB victim buffer, 32 MB aggressor window, 40 ms slots, 3 ms guard interval, five sweeps per slot, two warm-up slots. The payload is `M4MAX-SLC` with an `10101010` preamble.

**Run.**

```sh
clang -O2 -I. demos/poc2_slc_occupancy.c -o /tmp/poc2
/tmp/poc2
```

**Observed.** 80/80 bits decoded.

### 3. SMC power telemetry

**Mechanism.** The sender toggles CPU load to modulate package power. The receiver reads total package power from the System Management Controller through IOKit (`AppleSMC`, `IOConnectCallStructMethod`, key `PSTR`, little-endian float). The sensor updates at roughly 1 Hz, so each bit uses a 4 s slot.

**Method.** Idle and busy phases are alternated; the receiver thresholds samples at the midpoint between measured levels.

**Run.**

```sh
clang -O2 -I. -framework IOKit demos/poc3_smc_power.c -o /tmp/poc3
/tmp/poc3
```

**Observed.** 20/20 bits decoded (`M4`). Idle ≈ 20.25 W, busy ≈ 37.95 W, decision threshold ≈ 29.10 W.

### 4. Spectre v1 (Branch Different)

**Mechanism.** `array1` holds a public string immediately followed by a secret. The victim checks `index < *array_size` and, if in bounds, accesses `array2[array1[index] * 512]`. The attacker evicts `array2`, trains the branch with in-bounds indices, then presents an out-of-bounds index. The bounds check passes transiently, and the secret-dependent line of `array2` is cached. Probing `array2` reveals the value.

**What is upstream vs. what changed.** `spectre_m4/` is a modified copy of [Branch Different](https://github.com/cispa/BranchDifferent) (CISPA, DIMVA'22), MIT-licensed. The victim function, mistraining loop, eviction-set construction, cache-maintenance code, and counter-thread timer are upstream. The following files were changed for this machine:

| File | Change | Why |
|------|--------|-----|
| `spectre/config.h` | `ITERATIONS` 128, `BENCHMARK` 0, `VERBOSITY` 0 | Tuned for the M4 and the live demo |
| `spectre/main.c` | 1,023-byte public string; secret `M4SECRET`; output as lowercase hex | Removes the zero-nibble secret; enables byte-wise voting |
| `spectre/spectre.c` | Randomized, non-adjacent probe order; select the single fastest entry (excluding entry 0) instead of thresholding every entry | Defeats the hardware prefetcher that warms entries 2–15 at a 512 B stride |
| `leak.sh` | Five runs, per-byte majority vote, hex decode | Stabilizes a ~60–70 % per-run success rate |

**Why the upstream PoC does not work as-is on M4.**

1. **Offset-only L1 indexing.** With 16 KB pages and 128 B lines, the L1 set index is `(addr >> 7) & 127`: it depends only on the page offset. Eviction sets behave differently than on M1, and the upstream auto-calibrated threshold (~370 counter-thread units) sits above both L1 (~130) and L2 (~170), far below DRAM (~500+).
2. **Hardware prefetcher.** Sequential probing at a 512 B stride produces a broad hit plateau. The fix is the randomized, non-adjacent order plus an argmin vote.
3. **TAGE training.** The M4 branch predictor learns the fixed 9-training/1-attack pattern. Randomized training lengths were tried and made results worse in this build (partly because the attack call was not last in the loop); the fixed upstream pattern was kept.
4. **Eviction-set side effects.** Failed eviction-set searches register full 1,280-element lists (~20 MB traversal), which evict both L1 and L2 and actually enable the leak. Reduced sets evict only L1; forcing a full L1+L2 flush also evicts the secret's line and hurt results.
5. **Zero nibbles are unreadable.** Entry 0 is excluded (the training value maps to it), so any byte with a zero nibble is lost. `M4SPECTR` leaked as `M4S^ECTR` (7/8) because `0x50` contains a zero nibble; `M4SECRET` avoids the problem.

**Results.**

- **Live demo:** 8/8 bytes, `M4SECRET`, in ~15 s per invocation (five internal runs). Verified 4/4 independent invocations.
- **Reference benchmark (upstream binary, benchmark mode):** 60,457 / 81,920 correct bits (**73.80 %**), 10,240 bytes in 119,079 ms. An earlier run measured 74.88 %. At 73.8 %, roughly six bits per byte are recovered: a reliable nibble plus partial leakage of the second nibble.
- **Per-run success:** roughly 60–70 %; majority voting over five runs makes the demo stable.

**Run.**

```sh
make -C spectre_m4/spectre m1
./spectre_m4/leak.sh
# M4SECRET
```

The Makefile target invokes:

```sh
clang -I. -I../common -lpthread -pthread -lm ../common/*.c *.c -o spectre
```

### 5. DMP: negative result

A data memory-dependent prefetcher probe was attempted. No usable address-dependent signal was recovered. The likely explanation is DIT (Data Independent Timing) together with the prefetcher not exposing the required dependency; this was not independently verified. This is a negative finding for this machine, not evidence that the class is impossible elsewhere.

## Repository layout

```text
.
├── README.md                     this document
├── common.h                      shared timing, eviction, and page helpers
├── demos/
│   ├── poc1_cache_contention.c
│   ├── poc2_slc_occupancy.c
│   └── poc3_smc_power.c
└── spectre_m4/
    ├── leak.sh                   5-run majority-vote wrapper
    ├── LICENSE                   Branch Different MIT license
    ├── evsets.LICENSE            eviction-set code license
    ├── Readme.md                 upstream Branch Different readme
    ├── benchmark/                upstream
    ├── common/                   upstream
    ├── covert_channel/           upstream (runtime map file omitted)
    └── spectre/                  upstream + the M4 changes listed above
```

## Requirements

- Apple silicon Mac; tested on Apple M4 Max, 36 GB, macOS 15.7.3 (24G419).
- Xcode Command Line Tools (`clang`).
- Python 3 (only for `spectre_m4/leak.sh`).
- No root, kext, or special entitlement.

## Verification

Measured on 2026-10-20:

- Demo 1: 16/16 bytes.
- Demo 2: 80/80 bits (`M4MAX-SLC`).
- Demo 3: 20/20 bits (`M4`).
- Demo 4: 4/4 invocations produced `M4SECRET`.

## Novelty and prior art

I do not claim that any of this is novel, and I do not claim any "first on M4 Max". The techniques are established and M4 has already been studied:

- [Charting the Cache Side-Channel Frontier (USENIX Security '26)](https://www.usenix.org/conference/usenixsecurity26/presentation/wang-han) — systematic eviction-set construction on Apple M4, including the SLC victim buffer, DCP prefetcher, and allocation policy.
- [iEnFlow](https://github.com/CPU-Security/iEnFlow) — conditional branch predictor reverse engineering on M1–M4 and a Spectre-PHT attack.
- [SLAC (2026)](https://arxiv.org/abs/2608.09075) — CPU/GPU SLC side channel on Apple silicon.
- [The Power of Telemetry (2023)](https://arxiv.org/abs/2306.16391) — SMC power side channel on M1/M2.
- [Branch Different (DIMVA'22)](https://github.com/cispa/BranchDifferent) — Spectre v1/v2 and cache covert channels on Apple silicon.
- [iLeakage](https://ileakage.com/), [SLAP](https://yuval.yarom.org/pdfs/KimGY25.pdf), FLOP — speculative-execution attacks on M-series chips.

What is accurate to claim: an independent reproduction on this exact model and OS build, a working Branch Different port with documented M4 failure modes, original implementations of the three covert channels, and a negative DMP data point. Prefetch-aware probing, argmin selection, and majority voting are known techniques; the offset-only L1 indexing follows from 16 KB pages.

A genuinely novel result would require controlled experiments beyond a demo, for example a systematic M1-vs-M4 comparison of TAGE training behavior or a rigorous DMP characterization, plus a full prior-art review.

## Limitations

- One machine, one OS build; run-to-run variance is significant, especially for Spectre.
- The Spectre demo uses a cooperative local victim; it is not an end-to-end remote attack.
- The zero-nibble limitation is specific to this Branch Different configuration, not to Spectre v1 in general.
- The DMP result is a negative observation, not a proof of absence.

## Safety

These are local proof-of-concept attacks. Run them only on hardware you own or are authorized to test.

## Licensing and attribution

`spectre_m4/` is derived from Branch Different:

- Upstream: <https://github.com/cispa/BranchDifferent>, commit `9a622451cb596ecd0f036f4ad931c777120c16f6`.
- License: MIT, Copyright (c) 2022 CISPA. The original `LICENSE` and `evsets.LICENSE` are retained in `spectre_m4/`.
- Upstream `Readme.md`, `benchmark/`, `common/`, and `covert_channel/` are included for context; the runtime `covert_channel/shared` map file has been omitted.
- Changes are limited to the four files listed in the Spectre section and are documented there.

Demos 1–3 and the DMP probe are original code written for this project. No separate license is declared for that code.
