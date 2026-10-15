#!/usr/bin/env python3
"""Print Google Benchmark JSON results as a Markdown table.

Usage: bench_summary.py RESULTS.json [TITLE]

Reads the file that order_book_bench writes with --benchmark_out and prints, under TITLE, the
host's CPU count and caches, then one row per benchmark: the median of its repetitions when it
has several, otherwise its single run. Feed paths in benchmark names are cut to their file name,
so rows from different runners line up. Uses the standard library alone.
"""

from __future__ import annotations

import json
import re
import sys

# Google Benchmark's time units, as a multiple of a nanosecond
UNIT_NS = {"ns": 1.0, "us": 1e3, "ms": 1e6, "s": 1e9}


# The feed path at the start of a benchmark's arguments, with Unix or Windows separators
FEED_PATH = re.compile(r"^(?:.*?[/\\])?([^/\\]+\.(?:csv|itch|NASDAQ_ITCH50))")


def short_name(name: str) -> str:
    """Cut the feed path in a name such as BM_Replay/<path>/threads:1 to its file name."""
    benchmark, separator, arguments = name.partition("/")
    return benchmark + separator + FEED_PATH.sub(r"\1", arguments)


def time_text(nanoseconds: float) -> str:
    """Format a duration with a unit that keeps it between 1 and 1000."""
    for unit, scale in (("s", 1e9), ("ms", 1e6), ("us", 1e3)):
        if nanoseconds >= scale:
            return f"{nanoseconds / scale:.3g} {unit}"
    return f"{nanoseconds:.3g} ns"


def rows(benchmarks: list[dict]) -> list[dict]:
    """The median aggregate of each benchmark, or its only run when it was not repeated."""
    runs: dict[str, list[dict]] = {}
    for benchmark in benchmarks:
        runs.setdefault(benchmark.get("run_name", benchmark["name"]), []).append(benchmark)
    return [
        next((b for b in group if b.get("aggregate_name") == "median"), group[0])
        for group in runs.values()
    ]


def main(argv: list[str]) -> int:
    if len(argv) not in (2, 3):
        print(__doc__.strip().splitlines()[2], file=sys.stderr)
        return 2
    with open(argv[1], encoding="utf-8") as file:
        results = json.load(file)
    context = results.get("context", {})
    caches = ", ".join(
        f"L{cache['level']} {cache['type'].lower()} {cache['size'] // 1024} KiB"
        for cache in context.get("caches", [])
    )
    title = argv[2] if len(argv) == 3 else context.get("host_name", "benchmarks")
    print(f"### {title}\n")
    print(f"{context.get('num_cpus', '?')} CPUs; {caches or 'caches not reported'}\n")
    print("| Benchmark | Time | Items per second | Per message |")
    print("| --- | ---: | ---: | ---: |")
    for benchmark in rows(results.get("benchmarks", [])):
        scale = UNIT_NS.get(benchmark.get("time_unit", "ns"), 1.0)
        name = short_name(benchmark.get("run_name", benchmark["name"]))
        items = benchmark.get("items_per_second")
        per_message = benchmark.get("per_message")
        print(
            f"| `{name}` | {time_text(benchmark['real_time'] * scale)} | "
            f"{f'{items / 1e6:.2f} M' if items else ''} | "
            f"{time_text(per_message * 1e9) if per_message else ''} |"
        )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
