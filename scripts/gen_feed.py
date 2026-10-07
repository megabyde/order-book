#!/usr/bin/env python3
"""Generate a synthetic exchange event feed with the invariants of a real feed replay.

Usage: gen_feed.py [--events N] [--tickers K] [--seed S] [-o FILE]

The output has the same format as an exchange feed capture: a header, then
`Time,Ticker,Order,T,Shares,Price,MPID,X` rows with CRLF line endings. The same seed always
produces the same bytes.

The feed is internally consistent, as an exchange's own feed is: adds never cross the book,
C/D/E/F reference only live orders, E never exceeds the remaining quantity, C strictly decreases,
and every order still resting after the last random event is deleted, so each book ends empty.
E/F/C/D rows carry price 0; T rows carry order 0.
"""

from __future__ import annotations

import argparse
import random
import string
import sys
from dataclasses import dataclass, field

HEADER = "Time,Ticker,Order,T,Shares,Price,MPID,X"
TICK = 100  # One cent in the feed's 1/100-penny price units
MAX_OFFSET_TICKS = 20

# Message mix for a book with resting orders; an empty book always adds
WEIGHTS = {"add": 45, "D": 25, "C": 10, "E": 7, "F": 5, "T": 7, "X": 1}


@dataclass
class Book:
    mid: int
    ids: list[str] = field(default_factory=list)
    orders: dict[str, tuple[str, int, int]] = field(default_factory=dict)
    position: dict[str, int] = field(default_factory=dict)
    prices: dict[str, dict[int, int]] = field(default_factory=lambda: {"B": {}, "S": {}})

    def best_bid(self) -> int | None:
        return max(self.prices["B"], default=None)

    def best_ask(self) -> int | None:
        return min(self.prices["S"], default=None)

    def add(self, order: str, side: str, price: int, shares: int) -> None:
        self.orders[order] = (side, price, shares)
        self.position[order] = len(self.ids)
        self.ids.append(order)
        self.prices[side][price] = self.prices[side].get(price, 0) + 1

    def set_shares(self, order: str, shares: int) -> None:
        side, price, _ = self.orders[order]
        self.orders[order] = (side, price, shares)

    def remove(self, order: str) -> None:
        side, price, _ = self.orders.pop(order)
        index = self.position.pop(order)
        last = self.ids.pop()
        if last != order:
            self.ids[index] = last
            self.position[last] = index
        self.prices[side][price] -= 1
        if self.prices[side][price] == 0:
            del self.prices[side][price]


def ticker_name(index: int) -> str:
    letters = string.ascii_uppercase
    return letters[index // 676 % 26] + letters[index // 26 % 26] + letters[index % 26]


def generate(events: int, tickers: int, seed: int) -> list[str]:
    rng = random.Random(seed)
    books = {ticker_name(i): Book(mid=rng.randrange(1000, 5000) * TICK) for i in range(tickers)}
    names = list(books)
    actions = list(WEIGHTS)
    weights = list(WEIGHTS.values())
    rows = [HEADER]
    time = 34_200_000  # 09:30 in milliseconds since midnight
    next_order = 1

    def row(ticker: str, order: str, kind: str, shares: int, price: int) -> None:
        rows.append(f"{time},{ticker},{order},{kind},{shares},{price},,Q")

    for _ in range(events):
        time += rng.choice((0, 0, 1, 1, 2, 5, 20))
        ticker = rng.choice(names)
        book = books[ticker]
        if rng.random() < 0.05:
            book.mid = max(TICK * 10, book.mid + rng.choice((-TICK, TICK)))
        action = rng.choices(actions, weights)[0] if book.ids else "add"

        if action == "add":
            side = rng.choice("BS")
            offset = rng.randrange(0, MAX_OFFSET_TICKS) * TICK
            if side == "B":
                price = book.mid - TICK - offset
                ask = book.best_ask()
                if ask is not None:
                    price = min(price, ask - TICK)
            else:
                price = book.mid + TICK + offset
                bid = book.best_bid()
                if bid is not None:
                    price = max(price, bid + TICK)
            if price <= 0:
                continue
            shares = rng.choice((1, 10, 30, 50, 100, 100, 100, 200, 500, 1000))
            order = str(next_order)
            next_order += 1
            book.add(order, side, price, shares)
            row(ticker, order, side, shares, price)
            continue

        order = rng.choice(book.ids)
        _, price, shares = book.orders[order]
        if action == "D" or (action == "C" and shares == 1):
            book.remove(order)
            row(ticker, order, "D", 0, 0)
        elif action == "C":
            target = rng.randrange(0, shares)
            if target == 0:
                book.remove(order)
            else:
                book.set_shares(order, target)
            row(ticker, order, "C", target, 0)
        elif action == "E":
            executed = rng.randint(1, shares)
            if executed == shares:
                book.remove(order)
            else:
                book.set_shares(order, shares - executed)
            row(ticker, order, "E", executed, 0)
        elif action == "F":
            book.remove(order)
            row(ticker, order, "F", 0, 0)
        elif action == "T":
            row(ticker, "0", "T", rng.randint(1, shares), price)
        else:
            row(ticker, order, "X", 0, 0)

    time += 1
    for ticker, book in books.items():
        for order in list(book.ids):
            book.remove(order)
            row(ticker, order, "D", 0, 0)
    return rows


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--events", type=int, default=50_000, help="random events before drain")
    parser.add_argument("--tickers", type=int, default=8, help="number of tickers")
    parser.add_argument("--seed", type=int, default=1, help="random seed")
    parser.add_argument("-o", "--output", help="output file (default: stdout)")
    args = parser.parse_args()
    if args.events < 0 or not 1 <= args.tickers <= 26**3:
        parser.error("--events must be >= 0 and --tickers in [1, 17576]")

    data = "\r\n".join(generate(args.events, args.tickers, args.seed)) + "\r\n"
    if args.output:
        with open(args.output, "w", newline="") as out:
            out.write(data)
    else:
        sys.stdout.buffer.write(data.encode())
    return 0


if __name__ == "__main__":
    sys.exit(main())
