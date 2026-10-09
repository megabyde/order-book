#!/usr/bin/env python3
"""Replay an exchange event feed and print the best bid and offer whenever it changes.

Usage: reference_bbo.py [--stats] FILE

An independent model of the feed semantics, used as the oracle for the order_book application in
the differential test. It shares no code with the C++ implementation and keeps only aggregate
quantities per price level, which is all the BBO depends on.

Input: a header line, then `Time,Ticker,Order,T,Shares,Price[,...]` rows; extra columns, CRLF line
endings, and blank lines are accepted. Each BBO change prints
`<TIME>,<TICKER>,<BBP>,<BBQ>,<BAP>,<BAQ>` with empty fields for an empty side.

Messages, all referring to resting orders by ID:
  B/S  add a buy/sell order
  C    decrease the order *to* Shares; no-op unless Shares is smaller, removed at 0
  D    delete the order
  E    execute Shares of the order, clamped to what remains; removed at 0
  F    fill (remove) the order
  T/X  trades that do not touch the book

A malformed line, a duplicate order ID, or an unknown order ID is fatal: a message naming the line
goes to stderr and the exit status is 1. With --stats, feed-consistency counters go to stderr after
the replay.
"""

from __future__ import annotations

import argparse
import heapq
import re
import sys
from collections import Counter
from dataclasses import dataclass, field

UINT32_MAX = 2**32 - 1
UINT64_MAX = 2**64 - 1
DIGITS = re.compile(r"[0-9]+")
MESSAGE_TYPES = frozenset("BSCDEFTX")


class FeedError(Exception):
    """A line the replay cannot apply."""


def parse_uint(text: str, limit: int, name: str) -> int:
    if not DIGITS.fullmatch(text):
        raise FeedError(f"{name} is not an unsigned integer: '{text}'")
    value = int(text)
    if value > limit:
        raise FeedError(f"{name} out of range: '{text}'")
    return value


@dataclass
class Side:
    """Aggregate quantity per price for one side, with a lazily pruned heap for the best price."""

    sign: int  # -1 orders the heap by highest price first
    levels: dict[int, int] = field(default_factory=dict)
    heap: list[int] = field(default_factory=list)

    def add(self, price: int, quantity: int) -> None:
        if price not in self.levels:
            self.levels[price] = 0
            heapq.heappush(self.heap, self.sign * price)
        self.levels[price] += quantity

    def reduce(self, price: int, quantity: int) -> None:
        self.levels[price] -= quantity
        if self.levels[price] == 0:
            del self.levels[price]

    def best(self) -> tuple[int, int] | None:
        while self.heap and self.sign * self.heap[0] not in self.levels:
            heapq.heappop(self.heap)
        if not self.heap:
            return None
        price = self.sign * self.heap[0]
        return price, self.levels[price]


@dataclass
class Book:
    bids: Side = field(default_factory=lambda: Side(-1))
    asks: Side = field(default_factory=lambda: Side(1))
    orders: dict[str, tuple[str, int, int]] = field(default_factory=dict)

    def side(self, kind: str) -> Side:
        return self.bids if kind == "B" else self.asks

    def bbo(self) -> tuple[tuple[int, int] | None, tuple[int, int] | None]:
        return self.bids.best(), self.asks.best()

    def reduce(self, order: str, quantity: int) -> None:
        kind, price, remaining = self.orders[order]
        self.side(kind).reduce(price, quantity)
        if quantity == remaining:
            del self.orders[order]
        else:
            self.orders[order] = (kind, price, remaining - quantity)


def format_bbo(time: int, ticker: str, bbo: tuple) -> str:
    def level(entry: tuple[int, int] | None) -> str:
        return f"{entry[0]},{entry[1]}" if entry else ","

    return f"{time},{ticker},{level(bbo[0])},{level(bbo[1])}\n"


def replay(lines: list[str], out: list[str], stats: Counter) -> None:
    books: dict[str, Book] = {}
    for number, raw in enumerate(lines[1:], start=2):
        line = raw.removesuffix("\r")
        if not line.strip():
            continue
        fields = line.split(",")
        if len(fields) < 6:
            raise FeedError(f"line {number}: expected at least 6 fields, got {len(fields)}")
        try:
            time = parse_uint(fields[0], UINT64_MAX, "time")
            ticker, order, kind = fields[1], fields[2], fields[3]
            if kind not in MESSAGE_TYPES:
                raise FeedError(f"unknown message type '{kind}'")
            shares = parse_uint(fields[4], UINT32_MAX, "shares")
            price = parse_uint(fields[5], UINT32_MAX, "price")
        except FeedError as error:
            raise FeedError(f"line {number}: {error}") from None

        stats[f"type {kind}"] += 1
        book = books.setdefault(ticker, Book())
        before = book.bbo()

        if kind in "BS":
            if order in book.orders:
                raise FeedError(f"line {number}: duplicate order '{order}'")
            bid, ask = before
            crosses = ask and price >= ask[0] if kind == "B" else bid and price <= bid[0]
            if crosses:
                stats["crossing adds"] += 1
            book.orders[order] = (kind, price, shares)
            book.side(kind).add(price, shares)
        elif kind in "CDEF":
            if order not in book.orders:
                raise FeedError(f"line {number}: unknown order '{order}'")
            if price != 0:
                stats["C/D/E/F with nonzero price"] += 1
            remaining = book.orders[order][2]
            if kind == "C":
                if shares >= remaining:
                    stats["C not decreasing"] += 1
                else:
                    book.reduce(order, remaining - shares)
            elif kind == "E":
                if shares > remaining:
                    stats["E over remaining"] += 1
                book.reduce(order, min(shares, remaining))
            else:
                book.reduce(order, remaining)

        after = book.bbo()
        if after != before:
            out.append(format_bbo(time, ticker, after))

    stats["tickers"] = len(books)
    stats["resting orders at end"] = sum(len(book.orders) for book in books.values())


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("file", help="feed CSV, or - for stdin")
    parser.add_argument("--stats", action="store_true", help="print feed counters to stderr")
    args = parser.parse_args()

    if args.file == "-":
        lines = sys.stdin.read().split("\n")
    else:
        with open(args.file, newline="") as feed:
            lines = feed.read().split("\n")

    out: list[str] = []
    stats: Counter = Counter()
    try:
        replay(lines, out, stats)
    except FeedError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    finally:
        sys.stdout.write("".join(out))

    if args.stats:
        for key in sorted(stats):
            print(f"{key}: {stats[key]}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
