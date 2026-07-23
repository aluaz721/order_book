"""Feature-computation utilities that operate on order_book's own data types
(BookSnapshot, BookLevel, FillEvent, ...) rather than requiring a live book
or engine instance — usable against snapshots captured live, replayed from
history, or logged by a separate backtesting project.
"""

from . import microstructure

__all__ = ["microstructure"]
