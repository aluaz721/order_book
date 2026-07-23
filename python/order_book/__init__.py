"""
order_book — Python bindings for the C++ limit order book engine, plus pure-
Python utilities (currently: microstructure feature functions) that operate
on the data types the engine already produces (BookSnapshot, FillEvent, ...).

Everything from the compiled extension is re-exported here, so `TreeOrderBook`,
`Order`, `HistoricalReplayer`, `SimulationEngine`, etc. are all available
directly as `order_book.<name>` — the split between this package and the
`_order_book` submodule is an implementation detail.
"""

from ._order_book import *  # noqa: F401,F403
from . import features

__all__ = [name for name in dir() if not name.startswith("_")]
