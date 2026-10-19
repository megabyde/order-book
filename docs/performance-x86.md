# Linux x86 replay hillclimb

Combined median elapsed time fell **21.9%** against `3bf4f7e`, after the batch-wakeup fix. The
agreed 25% target was **not reached**. Eighteen source attempts retained three changes and
reverted fifteen. All final cases passed the 5% regression guard.

The retained source is `c28e914`. Later report and comment edits produce the same application
binary. The primary score is the geometric mean of candidate/reference median elapsed ratios for
long CSV and ITCH. Final score is 0.780505; all eight paired target scores improved (exact
one-sided sign test p = 0.00390625).

| Case        | Baseline median, s | Final median, s | Change | Baseline range, s | Final range, s |
| ----------- | -----------------: | --------------: | -----: | ----------------- | -------------- |
| Long CSV    |           0.948443 |        0.733381 | -22.7% | 0.8777–1.0755     | 0.7236–0.8477  |
| Long ITCH   |           0.483604 |        0.380999 | -21.2% | 0.4343–0.5751     | 0.3597–0.4721  |
| Broad CSV   |           0.070398 |        0.064348 |  -8.6% | 0.0665–0.0818     | 0.0585–0.0817  |
| Broad ITCH  |           0.088913 |        0.043273 | -51.3% | 0.0688–0.1076     | 0.0381–0.0461  |
| Single CSV  |           0.087786 |        0.086709 |  -1.2% | 0.0864–0.0936     | 0.0840–0.0878  |
| Single ITCH |           0.059320 |        0.046986 | -20.8% | 0.0585–0.0632     | 0.0451–0.0546  |

Broad and single-ticker cases are guards, excluded from the primary score. [Final raw
samples](data/x86-hillclimb-timings.csv), [all attempt
summaries](data/x86-hillclimb-attempts.csv), and [identities, input counts, output hashes, and
exact statistics](data/x86-hillclimb-metadata.json) accompany this report.

## Retained changes

- Cap Linux x86 CLI workers at eight. Public replay defaults and explicit worker counts stay
  unchanged.
- Skip the second ITCH prefetch stage on Linux x86; retain the first stage at 16 messages ahead.
- Use 256-order pages on Linux x86 with 16-bit live counts. Other platforms retain 128-order pages
  and 8-bit counts.

The page change has a measured memory cost. A separate single run per side on long ITCH raised
peak RSS from 112,484 to 121,580 KiB, about 8.1%, while the paired page trial reduced ITCH time by
4.5%. Broad ITCH peak RSS rose from 79,712 to 83,620 KiB; single ITCH fell from 15,632 to 14,404
KiB. These RSS observations are separate from timing samples.

## Attempts

Each row compares one source change with the current retained build. Percentages are not
cumulative. Every trial passed its release tests, lint, and complete output comparisons. Failed
timing gates revert the full candidate. Raw results, patches, profiles, and decision logs remain
in `.audit/hillclimb/`.

|   # | Change                  |    Time | Verdict  | Reason                   |
| --: | ----------------------- | ------: | -------- | ------------------------ |
|   1 | ITCH producer partition |  +8.23% | reverted | ITCH +18.9%; CPU lower   |
|   2 | Eight CLI workers       | -14.96% | kept     | All pairs/guards pass    |
|   3 | Sixteen CLI workers     |  -1.46% | reverted | No significant win       |
|   4 | Routing bitmask         |  +1.39% | reverted | No significant win       |
|   5 | CSV find/substr split   |  +3.67% | reverted | CSV regression           |
|   6 | 4 MiB batches           |  +6.16% | reverted | CSV + four guards        |
|   7 | Blank-line fast path    |  +2.28% | reverted | Median score worsened    |
|   8 | Remove level prefetch   |  -5.36% | kept     | ITCH -10.4%; guards pass |
|   9 | 256 KiB batches         | +12.06% | reverted | CSV and ITCH regress     |
|  10 | Eight-digit SWAR parse  |  -1.17% | reverted | Single CSV regresses     |
|  11 | Four CLI workers        | +18.79% | reverted | Target/broad regress     |
|  12 | No ITCH prefetch        |  +5.41% | reverted | ITCH +9.3%               |
|  13 | 256-order pages         |  -2.47% | kept     | ITCH -4.5%; guards pass  |
|  14 | Eight queued batches    |  +2.16% | reverted | No significant win       |
|  15 | 512-order pages         |  +0.06% | reverted | No significant win       |
|  16 | Two queued batches      | +10.90% | reverted | CSV and ITCH regress     |
|  17 | 32-message prefetch     |  +3.48% | reverted | ITCH +7.8%               |
|  18 | 8-message prefetch      |  +0.07% | reverted | No win; two guard flags  |

## Method

Intel Core i9-13900, 24 cores/32 logical CPUs, Linux x86-64. GCC 15.2.0, `-O3 -DNDEBUG`, generic
x86-64, repository hardening. LTO was requested but unavailable: the existing GCC/lld IPO probe
failed for baseline and every candidate, so actual compile/link commands omitted LTO. Compiler
flags, dependencies, affinity, and OS settings stayed fixed. The CLI worker cap is a source
change.

Each case uses eight alternating AB/BA pairs. Full CLI runs read warm files and write to
`/dev/null`. Separate validation compares complete output SHA-256, bytes, and lines before timing.
The harness checks frozen inputs, baseline, compiler/cache policy, dependency lock/toolchain, and
its own identity before and after each run. All samples remain recorded, including desktop
activity and scheduling variation.

The primary score is the geometric mean of per-target candidate/reference median elapsed ratios.
An exact one-sided sign test over paired target ratios must have p <= 0.05; score must improve and
every case median must remain <= 1.05 for automatic acceptance. Guards do not improve the primary
score.

Long feeds contain eight drained copies of a generated deep feed (4M requested events, 200
tickers, depth 4000, seed 2). CSV retains one header. Actual records are 36,186,432 CSV events and
36,188,048 ITCH frames. Both produce 8,497,112 BBO lines (316,345,368 bytes). Broad guards request
2M events/8000 tickers/depth4000/seed3; single guards request 1M events/1 ticker/depth4000/seed4.
Baseline outputs match the independent Python oracle.

Harness controls detected a deliberately slower wrapper and did not accept an A/A comparison
(score 0.9826, p 0.1445). Sixteen harness tests cover output errors, timeouts, identity changes,
locking, and paired statistics.

## Profiling and stopping point

Separate `perf record -e cpu-clock -F 997 -g --call-graph dwarf` runs identified CPU work. CSV
parsing accounted for 32.16% of samples and book application 17.04%. ITCH worker body accounted
for 32.85% and adds 13.86%; lookups, reductions, prefetch, and output remained visible. These are
CPU sample shares, not elapsed-time shares. Separate software `perf stat` runs measured
task-clock, context switches, CPU migrations, and page faults. Profilers did not overlap timing
runs.

The run stopped after bracketing retained worker counts, batch/queue sizes, page capacity, and
prefetch distance with losing alternatives, plus rejecting routing and parser rewrites. Further
parameter interpolation has weak evidence of enough gain to justify more trials. The best next
design experiment is to pass stable ticker IDs in CSV batches, avoiding repeated worker-side
ticker lookup, while checking whether producer work becomes the limiter. That requires a broader
handoff and book-lifetime design; its benefit is unmeasured.

## Arm documentation comparison

| Path             | Arm M1 Max, docs     | Linux x86, this run |
| ---------------- | -------------------- | ------------------- |
| CSV application  | 4.9–5.5 s            | 0.733 s median      |
| ITCH application | 5.19–5.33 s          | 0.381 s median      |
| ITCH one thread  | 20.6–20.8 s          | Not measured        |
| ITCH engine      | 53.2–55.7 ns/message | Not measured        |

Arm CSV uses 113.6M events and 8000 tickers; x86 uses 36.2M events and 200 tickers.
Arm ITCH uses a real 8.25 GB day; x86 uses a synthetic 1.18 GB feed.

Arm figures come from [the existing architecture
measurements](architecture.md#how-the-numbers-moved). Different inputs, machines, and concurrency
prevent an architecture speedup ratio or winner.

## Verification and limits

Release, ASan/UBSan, and TSan each passed 117 tests. Coverage passed at 100% of 767 lines and 125
functions. Sixteen harness tests passed. Output hashes match the independent Python oracle and
frozen baseline. No input capture or generated feed is committed.

Results describe synthetic, warm-cache CLI replay with output to `/dev/null`. They do not
establish a physical ceiling or real-capture speedup. Desktop activity and scheduling variation
remained in the samples. The 25% target remains unmet; the accepted improvement is 21.9%.

Reproduce the measurement protocol with [the paired hillclimb
instructions](development.md#paired-hillclimb-measurements). Generate the deep feed with `--events
4000000 --tickers 200 --depth 4000 --seed 2`, encode it with `scripts/csv_to_itch.py`, and
concatenate eight copies, keeping only the first CSV header. Freeze the post-wakeup baseline
before rebuilding candidates. The metadata file records exact input hashes.
