# Architecture

`order_book` reads a feed in batches on the calling thread, applies each batch on worker threads
that each own a share of the books, and writes the output back in feed order. The
[README](../README.md#design) lists the headers; [Benchmarks](development.md#benchmarks) shows how
to reproduce the numbers below.

## Data flow

```text
file ──► calling thread: read 1 MiB batches, frame records ──► queue of up to 4 batches
                                                                  │
            ┌──────────────────────────┬──────────────────────────┤
            ▼                          ▼                          ▼
        worker 0                   worker 1          ...      worker N-1
   its tickers' books         its tickers' books          its tickers' books
   lines for its records      lines for its records       lines for its records
            └──────────────────────────┴──────────────────────────┤
                                                                  ▼
             calling thread: merge the lines in record order, one write per batch ──► stdout
```

The application gives its input stream a 1 MiB buffer, since libc++'s `filebuf` otherwise reads a
few KiB per call; reading the 8.25 GB ITCH day took 1.73 s with the default buffer and 0.65 s with
the larger one. `replay()` reads at least 1 MiB at a time and cuts it at the last whole record: the
last LF for CSV, the end of the last complete message for ITCH, whose framing walk follows the
2-byte big-endian lengths and keeps where each message starts. A partial record carries over to the
next batch.

Each record has a number, its line or message number in the feed, which is what errors report and
what the merge orders the output by.

## Threads

A book belongs to one worker for the whole run. CSV lines go to the worker their ticker hashes to;
the calling thread splits each batch into lines, skips the header and blank lines, and hands every
worker the list of its own. ITCH messages go to the worker their stock locate selects, and there the
calling thread only frames them: every worker scans the batch's message starts and keeps the ones
whose locate is its own, without a branch per message, since most messages belong to another worker.
A message too short to name a locate goes to worker 0, which reports it.

Every worker applies every batch in order, so the workers drift apart by at most the four batches in
flight. A worker writes a line into its own buffer for each record that changes a BBO, tagged with
the record's number. When all workers have finished a batch, the calling thread marks which worker,
if any, wrote a line for each record, copies the lines out in record order, and writes the batch in
one call. The output therefore does not depend on the thread count; the tests compare a one-thread
replay with replays on up to 32 threads for both formats.

Workers notify the shared condition variable only when the last worker completes a batch.
An earlier completion changes neither the caller's completion predicate nor a worker's work
predicate. Queue insertion and shutdown still notify every waiter. Completion also notifies every
waiter because waking just one could select a worker instead of the caller.

On Linux x86-64 with an Intel Core i9-13900, this reduced CSV replay's median elapsed time from
1.021 s to 0.872 s, or 14.5%, across nine runs per build with alternating run order.
The ranges were 0.883–1.088 s before and 0.837–1.095 s after. The two-sided exact rank-sum
test gave p = 0.011. Median process CPU time fell from 8.514 s to 6.655 s, and voluntary context
switches from 432,346 to 85,490. Profiling found futex contention before the change.
Parsing, book updates, and output merging remain work that this change does not reduce.

These measurements used GCC 15.2.0, `-O3`, LTO, hardening, and the default 32 workers on both
builds. The input was eight copies of the generated 4M-event deep feed, with only the first CSV
header retained. Each copy ends with empty books, so concatenation preserves valid order lifetimes.
The CLI read warm files and wrote to `/dev/null`; separate untimed runs verified identical output
hashes and 8,497,112 output lines. Desktop processes remained active during measurement.
ITCH encoded from the same feed also produced identical output and fewer context switches, but its
elapsed-time difference was inconclusive. A separate `-march=native` trial showed no consistent
gain, so the release build retains generic x86-64 code generation. Synthetic feeds do not establish
performance on a real trading-day capture.

A record that fails stops its worker, whose books are unusable after a failed apply. The batch's
earliest failure across all workers ends the replay: the merge writes the lines before it, then
`replay()` throws with the record's number, so the output and the error are the same as with one
thread.

The streams can fail too. The calling thread checks the output stream after each batch and after
the flush that ends the replay, and throws if it failed, so a replay that returns has delivered
every line to the stream. A read that throws ends the input at the last whole record before it: the
batches read so far are applied and written, and the exception is rethrown after them unless one of
their records failed first. The read runs inside `std::string::resize_and_overwrite()`, which must
not see an exception, so the reader keeps the exception until the call returns.

The CLI uses one worker per hardware thread, capped at eight on Linux x86-64. `replay()` keeps its
explicit worker count and default unchanged. The ITCH day is CPU-bound from 8 workers on an M1 Max:
total CPU time grows from 28.4 s on one thread to 37.7 s on eight, and the two efficiency cores add
3.3 s of CPU without making the run shorter.

## The CSV book

`OrderBook` keeps one ticker's book with string order IDs, as the CSV format gives them. A level is
a pooled record with its total, price, order count, and side, and each side is a vector of prices and
level handles, worst first, so the best level is the last element and a change to it is a change to
the BBO. Finding a price scans from the best end, where most adds and removals happen. Orders live
in an `ankerl::unordered_dense::map` from order ID to the order's remaining quantity and its level's
handle, so an execution or delete reaches the level without a search. The map keeps its entries in
one vector, moving the last into the place of an erased one, and indexes them with Robin Hood
hashing up to 80% full; the books of a worker's tickers live in the same kind of map.

It replaced an open-addressing index with linear probing, kept at most half full, over a vector of
orders that reused removed positions. On one thread, the 113.6M-event synthetic feed replayed in
22.1–30.5 s instead of 27.7–35.5 s, 14–26% faster in each of six interleaved pairs, and the deep
feed in 510–547 ms instead of 572–628 ms. With all threads, the application took 4.9–5.5 s instead
of 5.4–6.2 s and peaked at 155–158 MiB instead of 170–173. Books small enough to stay in cache pay
for it: with 64 orders, `BM_AddDelete` runs 35–39% slower and `BM_ExecuteFront` 26–31%, and the
real one-ticker capture replays in 31.0–31.9 ms instead of 29.9–30.3. On a 10M-event feed over 8000
tickers, the map of books alone gave about 6.5% of the 13–17% there, and packing the old vector of
orders recovered 3%; the rest is the table, which linear probing did not match: filled to 3/4, it
replayed the capture 23% slower.

The levels were a `std::map` per side until the ITCH market's structure measured faster here too.
On one thread, the 113.6M-event synthetic feed replayed in 27.6–30.4 s instead of 31.0–31.8 s and
the deep feed in 547–605 ms instead of 592–663 ms; the shallow feed did not change.

## The ITCH market

The ITCH path first ran through the CSV book with integer IDs (#16), at 113–117 ns a message, and
has its own structure because the two formats name orders differently. A CSV order ID is a string
that only has to be unique within its ticker, so each ticker's book hashes it. An ITCH reference
number is an integer, unique for the day across all stocks and assigned in sequence, so a worker's
books for all its locates share one `ItchMarket`, which finds an order by shifting and masking its
reference number, without a hash.

Both books keep their levels in a `LevelPool` from `levels.hpp`. Sorted vectors with binary search
had lost 12% in the CSV book; searching from the best end, with orders holding a handle to a pooled
record, did not. `LevelPool::add()` handles only an order that joins an existing level, as most do,
and leaves a new level to `create()`, out of line, whose vector insert costs more than the call.
Written as one function, the compiler kept `add()` out of line once both books called it, and
`BM_ItchEngine` ran about 2% slower on the ITCH day: 51.9–54.1 ns per message against 50.6–52.9.

Decoding is a single dispatch: `visit_itch()` reads the type byte and calls the handler method for
the message's kind with the decoded fields, so the replay looks at the type once.

Orders live in an index of pages by reference number. A page holds 256 consecutive reference
numbers on Linux x86-64 and 128 elsewhere. Each order takes 8 bytes for its remaining shares and
the handle of its level. A two-level directory finds the page, made only for the ranges in use,
so one reference number near
2^32 costs one directory chunk rather than a table in every worker. Nasdaq assigns reference numbers
in sequence within each of several ranges at once, so the recent orders most messages name sit in a
few pages that stay in cache.

A level is a pooled 24-byte record with its total, price, order count, locate, and side, so an
execution or delete reaches it from the order without a search. Each side of a book is a vector of
prices and handles, worst first: the best level is the last element, and most adds and removals
touch only the end of the vector.

Each book keeps its own reference numbers, as a CSV book keeps its order IDs, so which locates share
a worker cannot change what a feed means. An order whose reference number another locate's live
order holds goes to a hash map keyed by both; on the real day it stays empty.

The index covers reference numbers below 2^32, which keeps a worker's directory within 2^16 chunk
pointers, and a lookup of a larger number falls past its end without a check of its own. ITCH gives
reference numbers 64 bits, so an order from 2^32 up goes to the same hash map. Nasdaq's numbers stay
far below the bound, the largest on 2019-12-30 being 308,631,072, but a feed that starts high still
replays, only slower. Only `add()` and `reduce()` test the bound. In four interleaved rounds on the
real day, `BM_ItchEngine` ran 0.1–0.8 ns a message slower than when the decoder rejected larger
numbers, about 0.7% and within the 0.9 ns spread of its repetitions, and the application's wall and
CPU time did not change.

The worker prefetches. Applying a message is a chain of dependent loads, from the index entry to the
level to the book side, and a message is several hundred instructions, about as many as the core
keeps in flight, so without help it waits for each miss in turn. Sixteen messages ahead, the worker
reads the message's locate and reference number with `peek_itch()` and starts loading the order's
index entry and the book. Linux x86_64 stops at this stage. Other platforms also prefetch eight
messages ahead, once those loads have arrived, to load the order's level and the book's two best
levels, which the BBO line reads. On one thread on the M1 Max, the two stages took the day from
24.6–24.9 s to 20.6–20.8 s, and any distance from 8 to 64 ran within 4%.

## Output

Each BBO line is formatted into the worker's buffer in one pass with `std::to_chars`, about 25 ns a
line. That is about 13% of a one-thread ITCH replay, and two attempts to cut it measured too small
to keep: writing lines through a raw buffer instead of `std::string` saved 1.8% of the replay's
instructions and no measurable time, and converting eight digits at a time saved 1.3 ns a line in a
microbenchmark.

## How the numbers moved

Every change below was measured on the same M1 Max, interleaved with the code before it, and kept
the 2019-12-30 ITCH day's output byte-identical.

- #17 put ITCH orders in the paged index, levels in the pool, and decoding in one dispatch.
- #19 let each ITCH worker pick its own messages, merged by owner, and gave the input a 1 MiB
  buffer.
- #20 added the prefetching.

| PR  | Measure                          |      Before |       After |
| --- | -------------------------------- | ----------: | ----------: |
| #17 | Decode and apply, ns per message |     113–117 |   54.0–54.1 |
| #19 | Application, all threads         | 8.16–8.33 s | 5.19–5.33 s |
| #20 | Decode and apply, ns per message |   60.9–61.6 |   55.5–56.5 |
| #20 | `replay()`, one thread           | 24.6–24.9 s | 20.6–20.8 s |

The two decode-and-apply rows come from different sessions: #17's review fixes added about 4% of
instructions and no measurable cycles, and most of the gap between 54 and 61 ns was the machine.

`BM_ItchEngine` has since moved to the workers' path: `visit_itch()` instead of decoding into a
message and applying it, and a BBO read after each change. In four interleaved rounds on the ITCH
day, the dispatch alone stayed within 0.7 ns a message of the old loop either way, and the BBO read
added 0.3–3.2 ns, so the benchmark reports 53.2–55.7 ns where the old loop reported 50.5–54.8.

For CSV, v0.1.0 replayed a synthetic 113.6M-event feed over 8000 tickers in 4.7 s with all threads
and 35 s with one worker, after block input and output (#8), a slab of orders with an
open-addressing index (#9), ticker sharding (#10), a single-pass parser (#11), and best-first levels
that report BBO changes (#13).

Several ideas lost when measured, and are not in the code:

- A `std::pmr` pool for the CSV book gave no gain at depth and 67% more peak memory, because
  libc++'s unsynchronized pool keeps freed bucket arrays.
- Abseil's `flat_hash_map` for the order index was 13% slower on a shallow feed, from tombstone
  rehashes under churn, and `btree_map` levels were 4–5% slower.
- Sorted vectors with binary search for the CSV levels were 12% slower on a shallow feed and no
  faster on ITCH.
- A pool that hands shards of locates to whichever thread is free ran no faster with one shard per
  thread and used 3–8 s more CPU with two or four, since the day is CPU-bound, not unbalanced.
- Moving the orders of sparse old pages into the hash map cut memory about as much as smaller
  pages do, but Nasdaq's concurrent ranges of reference numbers kept sending adds back to evicted
  pages, and one thread ran twice as slow.

## Memory

The ITCH market's memory is mostly pages. Each worker keeps only its own locates, so its orders are
a sparse subset of every range of reference numbers, and the few long-lived orders left in a range
keep its pages alive all day: at its peak, one market on the real day held 1.9M live orders in pages
of 256 reference numbers about 2% full. On the M1 Max, pages of 128 reference numbers, exactly 1 KiB
because their live counts sit in the directory, kept the application's peak at 1.11 GB with all
threads, against 1.90 GB with 256 and the count inside the page. The macOS allocator rounded those
2,052-byte pages up to 2,560 bytes. Linux x86-64 uses 256-reference, 2 KiB pages with 16-bit live
counts in the directory. Other platforms keep 128-reference pages and 8-bit counts.
