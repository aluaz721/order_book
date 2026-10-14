"""
order_book — Python bindings for the C++ limit order book engine, plus pure-
Python utilities (microstructure feature functions, and the Session façade)
that operate on the data types the engine already produces (BookSnapshot,
FillEvent, ...).

Everything from the compiled extension is re-exported here, so `TreeOrderBook`,
`Order`, `HistoricalReplayer`, `SimulationEngine`, etc. are all available
directly as `order_book.<name>` — the split between this package and the
`_order_book` submodule is an implementation detail.

Two ways in:

  `Session` wires up an engine, a book and an order queue for one symbol and
  takes orders of all seven types in dollars. Start here.

  The bound classes mirror the C++ API one-to-one (int64 basis points,
  explicit sources, ownership transfer on construction) for full control.
"""

from ._order_book import *  # noqa: F401,F403
from . import features
from .session import Session, OrderResult

__all__ = [name for name in dir() if not name.startswith("_")]
