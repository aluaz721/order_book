#!/usr/bin/env python3
"""Generate a synthetic NASDAQ ITCH 5.0 file for replay demos.

No ITCH sample file ships with this repo — NASDAQ's are large and separately
licensed — so the dashboard's replay mode needs something to point at. This
writes a small, deterministic file in the same wire format
`HistoricalReplayer` parses: [2-byte big-endian length][message].

Shape of the generated session
------------------------------
Adds are always passive: a bid below the best ask, an ask above the best bid.
That is how real ITCH behaves — an order that would cross never rests, so it
never appears as an Add; it shows up as executions against resting orders.
Generating it this way keeps the replayer's order-reference map in step with
the book the engine builds, which a crossing Add would desynchronise (the
book consumes the order, the replayer still believes it rests, and every
later message against that reference is dropped).

Trades therefore arrive as 'E' (Order Executed) against the resting order at
the touch, which is what an incoming aggressor would hit. The mid price takes
a random walk, so depth builds and drains on both sides over the session.

Message types emitted: 'A' (add), 'E' (execute), 'X' (partial cancel),
'D' (delete), 'U' (replace). All five are types the replayer handles.

Usage:
    python3 scripts/generate_itch.py --out data/synthetic_aapl.itch
"""

from __future__ import annotations

import argparse
import random
import struct
from dataclasses import dataclass, field
from pathlib import Path

# ITCH prices are integers in units of 1/10000 of a dollar, which is also the
# basis-point convention the engine uses internally.
PRICE_SCALE = 10_000

# 09:30:00 expressed as nanoseconds since midnight. ITCH carries no date;
# every timestamp in the file is an offset into the trading day.
MARKET_OPEN_NS = 9 * 3600 * 1_000_000_000 + 30 * 60 * 1_000_000_000


# ── Wire encoding ─────────────────────────────────────────────────────────────

def _u16(v: int) -> bytes:
    return struct.pack(">H", v)


def _u32(v: int) -> bytes:
    return struct.pack(">I", v)


def _u48(v: int) -> bytes:
    # Six-byte big-endian: the low 6 bytes of an 8-byte encoding.
    return struct.pack(">Q", v)[2:]


def _u64(v: int) -> bytes:
    return struct.pack(">Q", v)


def _stock(symbol: str) -> bytes:
    return symbol.ljust(8)[:8].encode("ascii")


def _framed(payload: bytes) -> bytes:
    return _u16(len(payload)) + payload


def msg_add(ref: int, side: str, shares: int, symbol: str,
            price: int, ts: int, locate: int = 1) -> bytes:
    """'A' — Add Order, no MPID. 36 bytes."""
    return _framed(
        b"A" + _u16(locate) + _u16(0) + _u48(ts) + _u64(ref)
        + side.encode("ascii") + _u32(shares) + _stock(symbol) + _u32(price))


def msg_execute(ref: int, shares: int, ts: int,
                match: int = 0, locate: int = 1) -> bytes:
    """'E' — Order Executed. 31 bytes."""
    return _framed(
        b"E" + _u16(locate) + _u16(0) + _u48(ts) + _u64(ref)
        + _u32(shares) + _u64(match))


def msg_cancel(ref: int, shares: int, ts: int, locate: int = 1) -> bytes:
    """'X' — Order Cancel, partial. Keeps time priority, prints no trade."""
    return _framed(
        b"X" + _u16(locate) + _u16(0) + _u48(ts) + _u64(ref) + _u32(shares))


def msg_delete(ref: int, ts: int, locate: int = 1) -> bytes:
    """'D' — Order Delete, full removal. 19 bytes."""
    return _framed(b"D" + _u16(locate) + _u16(0) + _u48(ts) + _u64(ref))


def msg_replace(old_ref: int, new_ref: int, shares: int,
                price: int, ts: int, locate: int = 1) -> bytes:
    """'U' — Order Replace. Side and symbol are inherited from the old ref."""
    return _framed(
        b"U" + _u16(locate) + _u16(0) + _u48(ts) + _u64(old_ref)
        + _u64(new_ref) + _u32(shares) + _u32(price))


# ── Book model ────────────────────────────────────────────────────────────────
#
# Just enough state to keep every generated message valid: which references
# are live, what they rest at, and where the touch is. Mirrors what the
# replayer itself tracks so the two never disagree.

@dataclass
class Resting:
    ref: int
    side: str        # 'B' or 'S'
    price: int
    shares: int


@dataclass
class Book:
    orders: dict[int, Resting] = field(default_factory=dict)

    def add(self, order: Resting) -> None:
        self.orders[order.ref] = order

    def remove(self, ref: int) -> None:
        self.orders.pop(ref, None)

    def side(self, side: str) -> list[Resting]:
        return [o for o in self.orders.values() if o.side == side]

    def best_bid(self) -> int | None:
        bids = self.side("B")
        return max((o.price for o in bids), default=None)

    def best_ask(self) -> int | None:
        asks = self.side("S")
        return min((o.price for o in asks), default=None)

    def at_touch(self, side: str) -> Resting | None:
        """Oldest resting order at the best price on `side` — FIFO front."""
        candidates = self.side(side)
        if not candidates:
            return None
        best = (max if side == "B" else min)(o.price for o in candidates)
        at_best = [o for o in candidates if o.price == best]
        return at_best[0] if at_best else None


def generate(symbol: str, seed: int, events: int,
             start_price: float, tick: float) -> bytes:
    rng = random.Random(seed)
    out = bytearray()
    book = Book()

    tick_i = int(round(tick * PRICE_SCALE))
    mid = int(round(start_price * PRICE_SCALE))
    ts = MARKET_OPEN_NS
    next_ref = 1

    def advance_clock() -> int:
        # Inter-event gaps: mostly sub-millisecond with an occasional pause,
        # so REALTIME replay has some texture rather than a metronome.
        nonlocal ts
        gap = rng.choice([
            rng.randint(50_000, 500_000),          # 50-500 microseconds
            rng.randint(1_000_000, 20_000_000),    # 1-20 milliseconds
            rng.randint(20_000_000, 90_000_000),   # a visible pause
        ])
        ts += gap
        return ts

    def claim_ref() -> int:
        nonlocal next_ref
        ref = next_ref
        next_ref += 1
        return ref

    def passive_price(side: str) -> int:
        """A price that cannot cross the opposite side."""
        depth = rng.randint(0, 6)
        if side == "B":
            ask = book.best_ask()
            price = mid - tick_i * (depth + 1)
            if ask is not None:
                price = min(price, ask - tick_i)
            return max(price, tick_i)
        bid = book.best_bid()
        price = mid + tick_i * (depth + 1)
        if bid is not None:
            price = max(price, bid + tick_i)
        return price

    def emit_add(side: str) -> None:
        ref = claim_ref()
        shares = rng.choice([100, 100, 200, 300, 500, 800, 1000])
        price = passive_price(side)
        out.extend(msg_add(ref, side, shares, symbol, price, advance_clock()))
        book.add(Resting(ref, side, price, shares))

    # ── Seed both sides so the book opens with visible depth ──────────────
    for _ in range(14):
        emit_add("B")
        emit_add("S")

    for _ in range(events):
        roll = rng.random()

        # Random-walk the mid so depth migrates over the session.
        if rng.random() < 0.18:
            mid += tick_i * rng.choice([-2, -1, -1, 1, 1, 2])

        if roll < 0.46:
            emit_add(rng.choice(["B", "S"]))

        elif roll < 0.72:
            # A trade: an incoming aggressor lifts/hits the touch.
            side = rng.choice(["B", "S"])
            target = book.at_touch(side)
            if target is None:
                emit_add(side)
                continue
            qty = min(target.shares, rng.choice([100, 100, 200, 300, 500]))
            out.extend(msg_execute(target.ref, qty, advance_clock(),
                                   match=claim_ref()))
            target.shares -= qty
            if target.shares == 0:
                book.remove(target.ref)

        elif roll < 0.84:
            # Partial cancel — reduces size, prints no trade.
            live = list(book.orders.values())
            if not live:
                continue
            target = rng.choice(live)
            if target.shares <= 100:
                continue
            qty = rng.randint(1, target.shares - 100)
            out.extend(msg_cancel(target.ref, qty, advance_clock()))
            target.shares -= qty

        elif roll < 0.94:
            # Full delete.
            live = list(book.orders.values())
            if len(live) <= 8:      # keep the book from emptying out
                continue
            target = rng.choice(live)
            out.extend(msg_delete(target.ref, advance_clock()))
            book.remove(target.ref)

        else:
            # Replace: reprice a resting order, still passively.
            live = list(book.orders.values())
            if not live:
                continue
            target = rng.choice(live)
            book.remove(target.ref)
            new_ref = claim_ref()
            price = passive_price(target.side)
            shares = rng.choice([100, 200, 300, 500])
            out.extend(msg_replace(target.ref, new_ref, shares, price,
                                   advance_clock()))
            book.add(Resting(new_ref, target.side, price, shares))

    return bytes(out)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, default=Path("data/synthetic_aapl.itch"))
    parser.add_argument("--symbol", default="AAPL")
    parser.add_argument("--events", type=int, default=3000,
                        help="order events after the opening depth is seeded")
    parser.add_argument("--seed", type=int, default=20260215,
                        help="fixed so the committed file is reproducible")
    parser.add_argument("--start-price", type=float, default=150.00)
    parser.add_argument("--tick", type=float, default=0.01)
    args = parser.parse_args()

    payload = generate(args.symbol, args.seed, args.events,
                       args.start_price, args.tick)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(payload)
    print(f"wrote {args.out} — {len(payload):,} bytes")


if __name__ == "__main__":
    main()
