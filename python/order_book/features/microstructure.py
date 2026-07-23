"""
Standard market-microstructure statistics computed from the order book's own
data types (BookSnapshot, BookLevel, FillEvent). None of these need a live
book reference — they work equally well against a snapshot captured live,
replayed from historical data, or logged by a separate backtesting project,
which is why they live here rather than downstream.

Units: prices and quantities are in the engine's own representation —
prices as int64 basis points (1 unit = $0.0001; see order_book.to_basis_points
/ order_book.from_basis_points to convert), quantities as raw share/contract
counts. Ratios (imbalance, relative spread) are dimensionless floats.
"""

from typing import Optional, Sequence

from .._order_book import BookLevel, BookSnapshot, FillEvent, Side

__all__ = [
    "mid_price",
    "spread",
    "relative_spread",
    "micro_price",
    "depth",
    "order_book_imbalance",
    "volume_weighted_price",
    "price_impact",
    "effective_spread",
]


def mid_price(snapshot: BookSnapshot) -> Optional[float]:
    """Simple mid-price: (best_bid + best_ask) / 2, in basis points.

    None if either side of the book is empty.
    """
    if not snapshot.bids or not snapshot.asks:
        return None
    return (snapshot.bids[0].price + snapshot.asks[0].price) / 2.0


def spread(snapshot: BookSnapshot) -> Optional[int]:
    """Best ask minus best bid, in basis points. None if either side is empty."""
    if not snapshot.bids or not snapshot.asks:
        return None
    return snapshot.asks[0].price - snapshot.bids[0].price


def relative_spread(snapshot: BookSnapshot) -> Optional[float]:
    """Spread normalized by mid price — dimensionless, comparable across
    instruments and price levels. None if either side is empty or mid is zero.
    """
    m = mid_price(snapshot)
    s = spread(snapshot)
    if m is None or s is None or m == 0:
        return None
    return s / m


def micro_price(snapshot: BookSnapshot) -> Optional[float]:
    """Queue-imbalance-weighted mid price (a.k.a. "microprice").

    Weights each side's price by the OPPOSITE side's resting quantity, so a
    thin ask against a heavy bid pulls the estimate up toward the ask (more
    likely to trade there next) and vice versa — a standard short-horizon
    price-direction signal (Stoikov 2018 and its many derivatives).

    None if either side of the book is empty.
    """
    if not snapshot.bids or not snapshot.asks:
        return None
    bid_price, bid_qty = snapshot.bids[0].price, snapshot.bids[0].quantity
    ask_price, ask_qty = snapshot.asks[0].price, snapshot.asks[0].quantity
    total = bid_qty + ask_qty
    if total == 0:
        return mid_price(snapshot)
    return (bid_price * ask_qty + ask_price * bid_qty) / total


def depth(levels: Sequence[BookLevel], max_levels: Optional[int] = None) -> int:
    """Total resting quantity across the top `max_levels` price levels (all
    levels if max_levels is None). Pass snapshot.bids or snapshot.asks.
    """
    if max_levels is not None:
        levels = levels[:max_levels]
    return sum(level.quantity for level in levels)


def order_book_imbalance(snapshot: BookSnapshot, max_levels: int = 1) -> Optional[float]:
    """Order flow imbalance across the top `max_levels` on each side:

        (bid_depth - ask_depth) / (bid_depth + ask_depth)

    Ranges from -1 (all resting quantity on the ask side) to +1 (all on the
    bid side) — a widely used short-term price-pressure signal. None if both
    sides are empty at that depth.
    """
    bid_depth = depth(snapshot.bids, max_levels)
    ask_depth = depth(snapshot.asks, max_levels)
    total = bid_depth + ask_depth
    if total == 0:
        return None
    return (bid_depth - ask_depth) / total


def volume_weighted_price(
    levels: Sequence[BookLevel], target_quantity: int
) -> Optional[float]:
    """Volume-weighted average price to fill `target_quantity` by walking
    `levels` from best to worst (pass snapshot.asks to price a buy,
    snapshot.bids to price a sell). None if the levels don't carry enough
    total quantity to fully satisfy target_quantity.
    """
    if target_quantity <= 0:
        raise ValueError("target_quantity must be positive")

    remaining = target_quantity
    notional = 0.0
    for level in levels:
        take = min(remaining, level.quantity)
        notional += take * level.price
        remaining -= take
        if remaining == 0:
            break

    if remaining > 0:
        return None  # not enough resting liquidity to fill target_quantity
    return notional / target_quantity


def price_impact(snapshot: BookSnapshot, side: Side, quantity: int) -> Optional[float]:
    """Estimated slippage (in basis points) of a hypothetical market order of
    `quantity`, relative to the current best price on the side it would
    execute against — Side.BID (buying) walks the ask side, Side.ASK
    (selling) walks the bid side. None if the book can't fill the full
    quantity.
    """
    levels = snapshot.asks if side == Side.BID else snapshot.bids
    if not levels:
        return None

    vwap = volume_weighted_price(levels, quantity)
    if vwap is None:
        return None

    best_price = levels[0].price
    return (vwap - best_price) if side == Side.BID else (best_price - vwap)


def effective_spread(fill: FillEvent, mid_price_at_fill: float) -> float:
    """Effective spread of a single fill, in basis points: twice the signed
    distance between the fill price and the prevailing mid price at the
    moment of the trade. Always non-negative for a well-formed cross.

    `mid_price_at_fill` must be supplied by the caller (compute it from the
    BookSnapshot in effect just before the fill, e.g. via mid_price()) since
    a FillEvent alone carries no book context.
    """
    sign = 1.0 if fill.aggressor_side == Side.BID else -1.0
    return 2.0 * sign * (fill.fill_price - mid_price_at_fill)
