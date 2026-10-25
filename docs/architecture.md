# Architecture

`order_book` reads a feed in batches on the calling thread, applies each batch on worker threads
that each own a share of the books, and writes the output back in feed order. The
[README](../README.md#design) lists the headers, and [Benchmarks](development.md#benchmarks) shows
how to reproduce the numbers below. Every measurement here is from one M1 Max, on the Nasdaq ITCH
day of 2019-12-30, the synthetic feeds of `scripts/gen_feed.py`, or a real one-ticker CSV capture,
with each variant interleaved with the code it was compared against.

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

`replay()` reads at least 1 MiB at a time and cuts it at the last whole record: the last LF for
CSV, and for ITCH the end of the last complete message, found by following the 2-byte big-endian
lengths, which also records where each message starts. A partial record carries over to the next
batch. The application gives its input stream a 1 MiB buffer as well, since libc++'s `filebuf`
otherwise reads a few KiB per call: reading the 8.25 GB ITCH day took 1.73 s with the default buffer
and 0.65 s with the larger one.

The calling thread refills the batch it last wrote, so the batch's text, its message starts, and the
workers' output buffers keep their capacity instead of being allocated and grown for every
megabyte; on the ITCH day that saves about 2% of the CPU time. Each record has a number, its line
or message number in the feed, which errors report and the merge orders the output by.

## Threads

A book belongs to one worker for the whole run. CSV lines go to the worker their ticker hashes to:
the calling thread splits each batch into lines, skips the header and blank lines, and hands every
worker the list of its own. ITCH messages go to the worker their stock locate selects, and the
calling thread only frames them; every worker scans the batch's message starts and keeps the ones
of its own locates, without a branch per message, since most messages belong to another worker. A
message too short to name a locate goes to worker 0, which reports it.

A multiplicative hash of the locate picks its worker. A locate's number says nothing about its
volume, and on the ITCH day the locate modulo 8 gave the busiest worker 23% more book messages than
the mean, worse than 94% of random assignments; the hash gives it 13%, about their median.

Every worker applies every batch in order, so the workers drift apart by at most the four batches in
flight. A worker writes a line into its own buffer for each record that changes a BBO, tagged with
the record's number. When all workers have finished a batch, the calling thread marks which worker,
if any, wrote a line for each record, copies the lines out in record order, and writes the batch in
one call. The output therefore does not depend on the thread count, and the tests compare one-thread
replays with replays on up to 32 threads in both formats. Queue insertion, batch completion, and
shutdown notify every waiter on the shared condition variable, since waking only one on completion
could wake a worker instead of the calling thread.

A record that fails stops its worker, whose books are unusable after a failed apply. The earliest
failure in the batch, across all workers, ends the replay: the merge writes the lines before it,
then `replay()` throws with the record's number, so the output and the error are the same as with
one thread.

The streams can fail too. The calling thread checks the output stream after each batch and after
the flush that ends the replay, and throws if it failed, so a replay that returns has delivered
every line to the stream. A read that throws ends the input at the last whole record before it: the
batches read so far are applied and written, and the exception is rethrown after them unless one of
their records failed first. The read runs inside `std::string::resize_and_overwrite()`, which must
not see an exception, so the reader holds the exception until the call returns.

The CLI and `replay()` default to one worker per hardware thread, at most eight; an explicit count
is not capped. The ITCH day is CPU-bound from 8 workers on the M1 Max, which has 8 performance and 2
efficiency cores: total CPU time grows from 23.7–23.8 s on one thread to 35.3–36.3 s on eight, and
ten workers add about 3 s of CPU without making the run shorter. Fewer workers trade time for CPU at
a steady rate: 4 workers took 6.67–6.71 s and 30.0–30.1 s of CPU, 6 took 4.77–4.80 s and 31.6–31.8 s.
The extra cycles are the cost of splitting the books, the scan and sparser pages, and the workers
slowing each other down through the shared caches and memory.

## Price levels

Both books keep their levels in a `LevelPool` from `levels.hpp`. A level is a pooled 16-byte record
with its total, order count, locate, and side, and each order holds its level's handle, so an
execution or delete reaches the level without a search. Each side of a book is a vector of prices
and handles, worst first, so the best level is the last element, a change to it is a change to the
BBO, and finding a price scans from the end, where most adds and removals happen. The price is kept
only in the vector: removing a level below the best finds its entry by handle, scanning from the end
as a search by price would, so four records fit in a cache line.

`LevelPool::add()` handles only an order that joins an existing level, as most do, and leaves a new
level to `create()`, out of line, whose vector insert costs more than the call. Written as one
function, `add()` stayed out of line once both books called it, and `BM_ItchEngine` ran about 2%
slower.

## The CSV book

`OrderBook` keeps one ticker's book with string order IDs, as the CSV format gives them. Orders live
in an `ankerl::unordered_dense::map` from order ID to the order's remaining shares and its level's
handle, 8 bytes. The map keeps its entries in one vector, moving the last into the place of an
erased one, and indexes them with Robin Hood hashing up to 80% full; a worker's books live in the
same kind of map, keyed by ticker.

## The ITCH market

An ITCH reference number is an integer, unique for the day across all stocks and assigned in
sequence, so a worker's books for all its locates share one `ItchMarket`, which finds an order by
shifting and masking its reference number, without a hash. Decoding is a single dispatch:
`visit_itch()` reads the type byte and calls the handler method for the message's kind with the
decoded fields.

Orders live in an index of pages by reference number. A page holds the orders of 128 consecutive
reference numbers, 8 bytes each: the order's remaining shares and its level's handle. A two-level
directory finds the page and exists only for the ranges in use, so one reference number near 2^32
costs one directory chunk rather than a table in every worker. Nasdaq assigns reference numbers in
sequence within each of several ranges at once, so the recent orders most messages name sit in a
few pages that stay in cache.

Each book keeps its own reference numbers, as a CSV book keeps its order IDs, so which locates share
a worker cannot change what a feed means. An order whose reference number another locate's live
order holds goes to a hash map keyed by both. The index covers reference numbers below 2^32, which
keeps a worker's directory within 2^16 chunk pointers, so a lookup of a larger number falls past its
end without a check of its own; ITCH gives reference numbers 64 bits, so an order from 2^32 up goes
to the same hash map. On the ITCH day the map stays empty, since the largest reference number is
308,631,072. Only `add()` and `reduce()` test the bound, which costs `BM_ItchEngine` about 0.7%,
within the spread of its repetitions.

The worker prefetches. Applying a message is a chain of dependent loads, from the index entry to the
level to the book side, and a message is several hundred instructions, about as many as the core
keeps in flight, so without help it waits for each miss in turn. Sixteen messages ahead, the worker
reads the message's locate and reference number with `peek_itch()` and starts loading the order's
index entry and the book. Other than on Linux x86-64, where it measured slower, it also prefetches
eight messages ahead, once those loads have arrived, the order's level and the book's two best
levels, which the BBO line reads. On one thread, the two stages took the ITCH day from 24.6–24.9 s
to 20.6–20.8 s, and any distance from 8 to 64 ran within 4%.

## Memory

The ITCH market's memory is mostly pages. Each worker keeps only its own locates, so its orders are
a sparse subset of every range of reference numbers, and the few long-lived orders left in a range
keep its pages alive all day. Pages of 128 reference numbers are exactly 1 KiB, because their live
counts sit in the directory. With ten workers they peaked at 1.11 GB, against 1.90 GB for pages of
256 with the count inside, which the macOS allocator rounded from 2,052 bytes up to 2,560.

## Widths and failures

Both feeds give an order's shares in 32 bits, and so do the event types and `ItchMessage`: the CSV
parser rejects a larger `Shares` field, and ITCH's fields are 4 bytes wide. A level's total is 64
bits, so the shares of fewer than 2^32 orders cannot overflow it. Level handles and each level's
order count are 32 bits as well, so a pool holds fewer than 2^32 levels at once and a level fewer
than 2^32 orders. Neither limit is checked: reaching one takes tens of GB of book state.

A `BookError` leaves a book as it was. An exception does not have to: running out of memory inside
an add can leave the order indexed without its level, so a book is not used after one, and
`replay()` stops at the first exception a worker reports. The CSV churn test and the ITCH market
fuzzer check every result against `MarketModel`: the error kind and an unchanged book for a rejected
event, and otherwise whether the BBO changed, the BBO itself, and the numbers of live orders and
levels.

## Output

Each BBO line is formatted into the worker's buffer in one pass with `std::to_chars`, about 25 ns a
line.

## Performance

The application with its output to `/dev/null` and the feed in the page cache, five interleaved
rounds with 8 workers and three with one:

| Feed                                      | Workers |          Wall |         CPU |        Peak |
| ----------------------------------------- | ------: | ------------: | ----------: | ----------: |
| ITCH day, 268.7M messages, 124.8M lines   |       8 |   4.29–4.88 s | 35.3–36.3 s | 962–969 MiB |
| ITCH day                                  |       1 | 19.62–19.73 s | 23.7–23.8 s |     510 MiB |
| Synthetic CSV, 113.6M events, 16.1M lines |       8 |   2.97–3.03 s | 24.3–24.5 s | 117–119 MiB |
| Synthetic CSV                             |       1 | 23.00–23.19 s | 25.2–25.4 s |     115 MiB |

Four of the five 8-worker ITCH rounds took 4.29–4.47 s. The synthetic feed is 25 runs of
`scripts/gen_feed.py --events 4000000 --tickers 8000`, seeds 101 to 125, joined under one header.
`BM_ItchEngine`, decoding and applying the ITCH day on one thread from memory, takes 51.6–52.0 ns a
message.

## Rejected

These lost when measured and are not in the code:

- **Routing ITCH through the CSV book** with integer IDs: 113–117 ns a message, decoded and applied
  on one thread, against 51.6–52.0 ns for `ItchMarket` in `BM_ItchEngine`.
- **Open addressing for CSV orders**, linear probing kept at most half full over a vector of orders
  that reused removed positions: on one thread the 113.6M-event synthetic feed took 27.7–35.5 s,
  against 22.1–30.5 s with `unordered_dense`, and with all threads 5.4–6.2 s and 170–173 MiB,
  against 4.9–5.5 s and 155–158 MiB. Books that stay in cache favor it: with 64 orders,
  `unordered_dense` runs `BM_AddDelete` 35–39% slower and `BM_ExecuteFront` 26–31%, and a real
  one-ticker capture replays in 31.0–31.9 ms against 29.9–30.3. Filled to 3/4 instead of half, it
  replayed that capture 23% slower.
- **A `std::map` per side for the CSV levels**: the 113.6M-event feed took 31.0–31.8 s on one thread
  against 27.6–30.4 s with the pool.
- **Sorted level vectors with binary search**: 12% slower on a shallow CSV feed, and no faster on
  ITCH.
- **Abseil**: `flat_hash_map` for CSV orders was 13% slower on a shallow feed, from tombstone
  rehashes under churn, and `btree_map` levels 4–5% slower.
- **A `std::pmr` pool for CSV orders**: no faster at depth, and 67% more peak memory, because
  libc++'s unsynchronized pool keeps freed bucket arrays.
- **Dynamic scheduling**, a pool that hands shards of locates to whichever thread is free: no faster
  with one shard per thread, and 3–8 s more CPU with two or four, since the day is CPU-bound.
- **A writer thread** that merges and writes, so the calling thread only reads and frames: the same
  4.56–4.64 s and 37.7 s of CPU.
- **Recording each message's worker while framing**, so the workers skip the scan: 11.5% fewer
  instructions, but 4.78–4.80 s instead of 4.56–4.58 s, because the calling thread then set the
  pace.
- **Evicting sparse pages** into the hash map: it saved about as much memory as smaller pages, but
  Nasdaq's concurrent ranges of reference numbers kept sending adds back to evicted pages, and one
  thread ran twice as slow.
- **Profile-guided optimization** with AppleClang, on top of ThinLTO: 5.0–5.5% fewer instructions
  on the ITCH day but only 1.5–2.1% fewer cycles, since the replay waits on memory, and on CSV at
  most 1% of cycles; too little for a two-stage build and a profile per compiler.
- **Faster formatting**: writing lines through a raw buffer instead of `std::string` saved 1.8% of
  the replay's instructions and no measurable time, and converting eight digits at a time saved
  1.3 ns a line in a microbenchmark.
