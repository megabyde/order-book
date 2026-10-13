#!/usr/bin/env python3
"""Encode a CSV exchange feed as Nasdaq TotalView-ITCH 5.0 with the same BBO trajectory.

Usage: csv_to_itch.py FILE [-o OUT]

FILE is a feed in the format reference_bbo.py reads, with numeric order IDs below 2**32 and tickers
of at most 8 characters. The output is length-prefixed ITCH 5.0 messages, so
`order_book --format=itch OUT` prints the same lines as `order_book FILE`: the timestamp field
carries the CSV time unchanged, and the stock field the ticker.

The encoding varies to cover every message type the replay decodes. Adds alternate between A and F,
and executions between E and C. A decrease to a nonzero quantity alternates between X, which cancels
the difference, and U, which replaces the order by one under a new reference number that later
messages then use. A decrease to 0 cancels the remainder, F executes it, and T and X become P and Q.
Each ticker gets a stock directory message before its first event, and system event messages open
and close the feed; the replay skips both.
"""

from __future__ import annotations

import argparse
import contextlib
import struct
import sys

UINT48_MAX = 2**48 - 1
# The replay rejects order reference numbers from 2**32 up
MAX_ITCH_ORDER = 2**32


class EncodeError(Exception):
    """A CSV line this encoder cannot represent."""


class Encoder:
    def __init__(self, used: set[int]) -> None:
        self.out: list[bytes] = []
        self.locates: dict[str, int] = {}
        # (ticker, CSV order ID) -> [reference number, remaining shares, price]
        self.orders: dict[tuple[str, str], list] = {}
        self.adds = self.executions = self.decreases = 0
        self.match = 0
        # Replacements count down from the top of the range, skipping every order ID of the feed,
        # so a replacement never takes a reference number a later add uses
        self.used = used
        self.replacement = MAX_ITCH_ORDER

    def message(self, body: bytes) -> None:
        self.out.append(struct.pack(">H", len(body)) + body)

    @staticmethod
    def header(kind: str, locate: int, time: int) -> bytes:
        return kind.encode() + struct.pack(">HH", locate, 0) + time.to_bytes(6, "big")

    @staticmethod
    def stock(ticker: str) -> bytes:
        return ticker.encode().ljust(8)

    def locate(self, ticker: str, time: int) -> int:
        if ticker not in self.locates:
            if len(ticker) > 8 or not ticker.isascii():
                raise EncodeError(f"ticker '{ticker}' does not fit ITCH's 8 ASCII characters")
            locate = len(self.locates) + 1
            self.locates[ticker] = locate
            directory = (
                self.header("R", locate, time) + self.stock(ticker) + b"QN" + struct.pack(">I", 100)
            )
            self.message(directory + b"NCZ PNN1N" + struct.pack(">I", 0) + b"N")
        return self.locates[ticker]

    def next_replacement(self) -> int:
        self.replacement -= 1
        while self.replacement in self.used:
            self.replacement -= 1
        if self.replacement <= 0:
            raise EncodeError("no reference number left for a replacement")
        return self.replacement

    def next_match(self) -> int:
        self.match += 1
        return self.match

    def execute(self, locate: int, time: int, order: list, shares: int) -> None:
        reference = struct.pack(">QI", order[0], shares) + struct.pack(">Q", self.next_match())
        if self.executions % 2 == 0:
            self.message(self.header("E", locate, time) + reference)
        else:
            price = struct.pack(">I", order[2])
            self.message(self.header("C", locate, time) + reference + b"Y" + price)
        self.executions += 1
        order[1] -= shares

    def cancel(self, locate: int, time: int, order: list, shares: int) -> None:
        self.message(self.header("X", locate, time) + struct.pack(">QI", order[0], shares))
        order[1] -= shares

    def row(self, fields: list[str]) -> None:
        if len(fields) < 6:
            raise EncodeError(f"expected at least 6 fields, got {len(fields)}")
        time, ticker, order_id, kind = int(fields[0]), fields[1], fields[2], fields[3]
        shares, price = int(fields[4]), int(fields[5])
        if time > UINT48_MAX:
            raise EncodeError(f"time {time} does not fit ITCH's 48-bit timestamp")
        locate = self.locate(ticker, time)

        if kind in "BS":
            reference = int(order_id)
            if (ticker, order_id) in self.orders or not 0 < reference < MAX_ITCH_ORDER:
                raise EncodeError(f"order ID '{order_id}' is live or not below {MAX_ITCH_ORDER}")
            self.orders[ticker, order_id] = [reference, shares, price]
            body = struct.pack(">Q", reference) + kind.encode() + struct.pack(">I", shares)
            body += self.stock(ticker) + struct.pack(">I", price)
            if self.adds % 2 == 0:
                self.message(self.header("A", locate, time) + body)
            else:
                self.message(self.header("F", locate, time) + body + b"TEST")
            self.adds += 1
        elif kind in "CDEF":
            order = self.orders.get((ticker, order_id))
            if order is None:
                raise EncodeError(f"unknown order '{order_id}'")
            remaining = order[1]
            if kind == "C" and shares < remaining:
                if shares > 0 and self.decreases % 2 == 1:
                    replacement = self.next_replacement()
                    body = struct.pack(">QQII", order[0], replacement, shares, order[2])
                    self.message(self.header("U", locate, time) + body)
                    order[0], order[1] = replacement, shares
                else:
                    self.cancel(locate, time, order, remaining - shares)
                self.decreases += 1
            elif kind == "D":
                self.message(self.header("D", locate, time) + struct.pack(">Q", order[0]))
                order[1] = 0
            elif kind == "E":
                self.execute(locate, time, order, min(shares, remaining))
            elif kind == "F":
                self.execute(locate, time, order, remaining)
            if order[1] == 0:
                del self.orders[ticker, order_id]
        elif kind == "T":
            body = struct.pack(">Q", 0) + b"B" + struct.pack(">I", shares) + self.stock(ticker)
            body += struct.pack(">IQ", price, self.next_match())
            self.message(self.header("P", locate, time) + body)
        elif kind == "X":
            body = struct.pack(">Q", shares) + self.stock(ticker)
            body += struct.pack(">IQ", price, self.next_match()) + b"C"
            self.message(self.header("Q", locate, time) + body)
        else:
            raise EncodeError(f"unknown message type '{kind}'")


def order_ids(lines: list[str]) -> set[int]:
    """The numeric order IDs of every add in `lines`, ignoring what row() rejects anyway."""
    ids = set()
    for line in lines[1:]:
        fields = line.split(",")
        if len(fields) > 3 and fields[3] in ("B", "S"):
            with contextlib.suppress(ValueError):
                ids.add(int(fields[2]))
    return ids


def encode(lines: list[str]) -> bytes:
    encoder = Encoder(order_ids(lines))
    encoder.message(Encoder.header("S", 0, 0) + b"O")
    last_time = 0
    for number, raw in enumerate(lines[1:], start=2):
        line = raw.removesuffix("\r")
        if not line.strip():
            continue
        try:
            fields = line.split(",")
            encoder.row(fields)
            last_time = int(fields[0])
        except (EncodeError, ValueError) as error:
            raise EncodeError(f"line {number}: {error}") from None
    encoder.message(Encoder.header("S", 0, last_time) + b"C")
    return b"".join(encoder.out)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("file", help="CSV feed")
    parser.add_argument("-o", "--output", help="output file (default: stdout)")
    args = parser.parse_args()

    with open(args.file, newline="") as feed:
        lines = feed.read().split("\n")
    try:
        data = encode(lines)
    except EncodeError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    if args.output:
        with open(args.output, "wb") as out:
            out.write(data)
    else:
        sys.stdout.buffer.write(data)
    return 0


if __name__ == "__main__":
    sys.exit(main())
