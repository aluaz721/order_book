"""
Stateless order validators — the same checks SimulationEngine runs on every new order before routing it to the book or the stop manager.
"""
from __future__ import annotations
import order_book._order_book
__all__: list[str] = ['validate', 'validate_fields', 'validate_fok', 'validate_stop']
def validate(order: order_book._order_book.Order, book: order_book._order_book.OrderBookInterface) -> order_book._order_book.ValidationResult:
    """
    Run every applicable validator: fields, then FOK feasibility or stop-price sanity by type.
    """
def validate_fields(order: order_book._order_book.Order) -> order_book._order_book.ValidationResult:
    """
    Structural checks only — no book access.
    """
def validate_fok(order: order_book._order_book.Order, book: order_book._order_book.OrderBookInterface) -> order_book._order_book.ValidationResult:
    """
    Whether a FILL_OR_KILL order can be filled in full right now. A pure read of the book.
    """
def validate_stop(order: order_book._order_book.Order, book: order_book._order_book.OrderBookInterface) -> order_book._order_book.ValidationResult:
    """
    Stop price on the correct side of the market, and (for STOP_LIMIT) limit_price consistent with the stop price: <= stop for buys, >= stop for sells.
    """
