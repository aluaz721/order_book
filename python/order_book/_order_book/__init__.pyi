"""
Python bindings for the C++ limit order book engine
"""
from __future__ import annotations
import collections.abc
import typing
from . import validators
__all__: list[str] = ['AckEvent', 'BASIS_POINTS_PER_UNIT', 'BookLevel', 'BookSnapshot', 'CancelEvent', 'CancelReason', 'FIFOMatcher', 'FillEvent', 'HistoricalReplayer', 'HistoricalReplayerConfig', 'InteractiveSource', 'MatchingAlgorithm', 'Order', 'OrderBookCallbacks', 'OrderBookInterface', 'OrderSource', 'OrderStatus', 'OrderType', 'ReplaySpeed', 'Side', 'SimulationConfig', 'SimulationEngine', 'StopOrderManager', 'StrategySource', 'TreeOrderBook', 'TriggerEvent', 'ValidationResult', 'format_price', 'from_basis_points', 'opposite', 'tick_ceil', 'tick_floor', 'to_basis_points', 'validators']
class Side:
    """
    Members:
    
      BID
    
      ASK
    """
    ASK: typing.ClassVar[Side]
    BID: typing.ClassVar[Side]
    __members__: typing.ClassVar[dict[str, Side]]
    def __eq__(self, other: typing.Any) -> bool:
        ...
    def __getstate__(self) -> int:
        ...
    def __hash__(self) -> int:
        ...
    def __index__(self) -> int:
        ...
    def __init__(self, value: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def __int__(self) -> int:
        ...
    def __ne__(self, other: typing.Any) -> bool:
        ...
    def __repr__(self) -> str:
        ...
    def __setstate__(self, state: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def __str__(self) -> str:
        ...
    @property
    def name(self) -> str:
        ...
    @property
    def value(self) -> int:
        ...
class OrderType:
    """
    Members:
    
      LIMIT
    
      MARKET
    
      MARKET_LIMIT
    
      STOP
    
      STOP_LIMIT
    
      IMMEDIATE_OR_CANCEL
    
      FILL_OR_KILL
    """
    FILL_OR_KILL: typing.ClassVar[OrderType]
    IMMEDIATE_OR_CANCEL: typing.ClassVar[OrderType]
    LIMIT: typing.ClassVar[OrderType]
    MARKET: typing.ClassVar[OrderType]
    MARKET_LIMIT: typing.ClassVar[OrderType]
    STOP: typing.ClassVar[OrderType]
    STOP_LIMIT: typing.ClassVar[OrderType]
    __members__: typing.ClassVar[dict[str, OrderType]]
    def __eq__(self, other: typing.Any) -> bool:
        ...
    def __getstate__(self) -> int:
        ...
    def __hash__(self) -> int:
        ...
    def __index__(self) -> int:
        ...
    def __init__(self, value: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def __int__(self) -> int:
        ...
    def __ne__(self, other: typing.Any) -> bool:
        ...
    def __repr__(self) -> str:
        ...
    def __setstate__(self, state: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def __str__(self) -> str:
        ...
    @property
    def name(self) -> str:
        ...
    @property
    def value(self) -> int:
        ...
class OrderStatus:
    """
    Members:
    
      NEW
    
      PENDING_TRIGGER
    
      PARTIALLY_FILLED
    
      FILLED
    
      CANCELLED
    
      REJECTED
    """
    CANCELLED: typing.ClassVar[OrderStatus]
    FILLED: typing.ClassVar[OrderStatus]
    NEW: typing.ClassVar[OrderStatus]
    PARTIALLY_FILLED: typing.ClassVar[OrderStatus]
    PENDING_TRIGGER: typing.ClassVar[OrderStatus]
    REJECTED: typing.ClassVar[OrderStatus]
    __members__: typing.ClassVar[dict[str, OrderStatus]]
    def __eq__(self, other: typing.Any) -> bool:
        ...
    def __getstate__(self) -> int:
        ...
    def __hash__(self) -> int:
        ...
    def __index__(self) -> int:
        ...
    def __init__(self, value: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def __int__(self) -> int:
        ...
    def __ne__(self, other: typing.Any) -> bool:
        ...
    def __repr__(self) -> str:
        ...
    def __setstate__(self, state: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def __str__(self) -> str:
        ...
    @property
    def name(self) -> str:
        ...
    @property
    def value(self) -> int:
        ...
class CancelReason:
    """
    Members:
    
      CLIENT_REQUEST
    
      IOC_EXPIRED
    
      FOK_FAILED
    
      MARKET_NO_LIQUIDITY
    
      RISK_REJECTED
    
      STOP_CANCELLED
    
      SELF_TRADE
    """
    CLIENT_REQUEST: typing.ClassVar[CancelReason]
    FOK_FAILED: typing.ClassVar[CancelReason]
    IOC_EXPIRED: typing.ClassVar[CancelReason]
    MARKET_NO_LIQUIDITY: typing.ClassVar[CancelReason]
    RISK_REJECTED: typing.ClassVar[CancelReason]
    SELF_TRADE: typing.ClassVar[CancelReason]
    STOP_CANCELLED: typing.ClassVar[CancelReason]
    __members__: typing.ClassVar[dict[str, CancelReason]]
    def __eq__(self, other: typing.Any) -> bool:
        ...
    def __getstate__(self) -> int:
        ...
    def __hash__(self) -> int:
        ...
    def __index__(self) -> int:
        ...
    def __init__(self, value: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def __int__(self) -> int:
        ...
    def __ne__(self, other: typing.Any) -> bool:
        ...
    def __repr__(self) -> str:
        ...
    def __setstate__(self, state: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def __str__(self) -> str:
        ...
    @property
    def name(self) -> str:
        ...
    @property
    def value(self) -> int:
        ...
class Order:
    side: Side
    status: OrderStatus
    symbol: str
    type: OrderType
    def __init__(self, id: typing.SupportsInt | typing.SupportsIndex, symbol: str, side: Side, type: OrderType, price: typing.SupportsInt | typing.SupportsIndex, quantity: typing.SupportsInt | typing.SupportsIndex, timestamp: typing.SupportsInt | typing.SupportsIndex = 0, limit_price: typing.SupportsInt | typing.SupportsIndex = 0, status: OrderStatus = ...) -> None:
        ...
    def __repr__(self) -> str:
        ...
    @property
    def id(self) -> int:
        ...
    @id.setter
    def id(self, arg0: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    @property
    def limit_price(self) -> int:
        ...
    @limit_price.setter
    def limit_price(self, arg0: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    @property
    def orig_quantity(self) -> int:
        ...
    @orig_quantity.setter
    def orig_quantity(self, arg0: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    @property
    def price(self) -> int:
        ...
    @price.setter
    def price(self, arg0: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    @property
    def quantity_remaining(self) -> int:
        ...
    @quantity_remaining.setter
    def quantity_remaining(self, arg0: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    @property
    def timestamp(self) -> int:
        ...
    @timestamp.setter
    def timestamp(self, arg0: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
class FillEvent:
    @property
    def aggressive_order_id(self) -> int:
        ...
    @property
    def aggressor_side(self) -> Side:
        ...
    @property
    def fill_price(self) -> int:
        ...
    @property
    def fill_quantity(self) -> int:
        ...
    @property
    def passive_order_id(self) -> int:
        ...
    @property
    def sequence(self) -> int:
        ...
    @property
    def symbol(self) -> str:
        ...
    @property
    def timestamp(self) -> int:
        ...
class CancelEvent:
    @property
    def order_id(self) -> int:
        ...
    @property
    def price(self) -> int:
        ...
    @property
    def reason(self) -> CancelReason:
        ...
    @property
    def remaining_quantity(self) -> int:
        ...
    @property
    def sequence(self) -> int:
        ...
    @property
    def side(self) -> Side:
        ...
    @property
    def symbol(self) -> str:
        ...
    @property
    def timestamp(self) -> int:
        ...
class TriggerEvent:
    @property
    def converted_to(self) -> OrderType:
        ...
    @property
    def side(self) -> Side:
        ...
    @property
    def stop_order_id(self) -> int:
        ...
    @property
    def stop_price(self) -> int:
        ...
    @property
    def symbol(self) -> str:
        ...
    @property
    def timestamp(self) -> int:
        ...
    @property
    def trigger_trade_price(self) -> int:
        ...
    @property
    def triggered_order_id(self) -> int:
        ...
class AckEvent:
    @property
    def message(self) -> str:
        ...
    @property
    def order_id(self) -> int:
        ...
    @property
    def status(self) -> OrderStatus:
        ...
    @property
    def symbol(self) -> str:
        ...
    @property
    def timestamp(self) -> int:
        ...
class BookLevel:
    @property
    def order_count(self) -> int:
        ...
    @property
    def price(self) -> int:
        ...
    @property
    def quantity(self) -> int:
        ...
class BookSnapshot:
    @property
    def asks(self) -> list[BookLevel]:
        ...
    @property
    def bids(self) -> list[BookLevel]:
        ...
    @property
    def last_trade_price(self) -> int:
        ...
    @property
    def sequence(self) -> int:
        ...
    @property
    def symbol(self) -> str:
        ...
    @property
    def timestamp(self) -> int:
        ...
class OrderBookCallbacks:
    on_ack: collections.abc.Callable[[AckEvent], None]
    on_book_update: collections.abc.Callable[[BookSnapshot], None]
    on_cancel: collections.abc.Callable[[CancelEvent], None]
    on_fill: collections.abc.Callable[[FillEvent], None]
    def __init__(self) -> None:
        ...
class MatchingAlgorithm:
    pass
class FIFOMatcher(MatchingAlgorithm):
    def __init__(self) -> None:
        ...
    def name(self) -> str:
        ...
class OrderBookInterface:
    pass
class TreeOrderBook(OrderBookInterface):
    def __init__(self, symbol: str, matcher: MatchingAlgorithm, callbacks: OrderBookCallbacks = ...) -> None:
        ...
    def add(self, order: Order) -> None:
        ...
    def ask_depth(self) -> int:
        ...
    def best_ask(self) -> int | None:
        ...
    def best_bid(self) -> int | None:
        ...
    def bid_depth(self) -> int:
        ...
    def cancel(self, order_id: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def execute(self, order_id: typing.SupportsInt | typing.SupportsIndex, quantity: typing.SupportsInt | typing.SupportsIndex, timestamp: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def has_order(self, order_id: typing.SupportsInt | typing.SupportsIndex) -> bool:
        ...
    def last_trade_price(self) -> int:
        ...
    def mid_price(self) -> float | None:
        ...
    def reduce(self, order_id: typing.SupportsInt | typing.SupportsIndex, quantity: typing.SupportsInt | typing.SupportsIndex, timestamp: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def replace(self, old_order_id: typing.SupportsInt | typing.SupportsIndex, new_order: Order) -> None:
        ...
    def sequence(self) -> int:
        ...
    def set_callbacks(self, callbacks: OrderBookCallbacks) -> None:
        ...
    def set_matching_algorithm(self, algo: MatchingAlgorithm) -> None:
        ...
    def snapshot(self, depth: typing.SupportsInt | typing.SupportsIndex = 10) -> BookSnapshot:
        ...
    def spread(self) -> int | None:
        ...
    def symbol(self) -> str:
        ...
    def total_ask_qty(self) -> int:
        ...
    def total_bid_qty(self) -> int:
        ...
    def weighted_mid(self) -> float | None:
        ...
class ValidationResult:
    def __bool__(self) -> bool:
        ...
    def __repr__(self) -> str:
        ...
    @property
    def reason(self) -> str:
        ...
    @property
    def valid(self) -> bool:
        ...
class StopOrderManager:
    def __init__(self, book: OrderBookInterface, on_trigger: collections.abc.Callable[[TriggerEvent], None] = None, on_cancel: collections.abc.Callable[[CancelEvent], None] = None, next_order_id: collections.abc.Callable[[], int] = None) -> None:
        ...
    def cancel(self, order_id: typing.SupportsInt | typing.SupportsIndex, timestamp: typing.SupportsInt | typing.SupportsIndex = 0) -> None:
        """
        Cancel a stop that has not triggered yet. A no-op if it has already fired or is unknown.
        """
    def drain_triggers(self) -> None:
        """
        Act on every recorded trade, submitting the orders any crossed stops convert into. Call only after the book operation that produced those trades has returned.
        """
    def has_stop(self, order_id: typing.SupportsInt | typing.SupportsIndex) -> bool:
        ...
    def nearest_stop_buy(self) -> int:
        """
        Lowest pending buy-stop price (the next to trigger as the market rises), or 0 if none.
        """
    def nearest_stop_sell(self) -> int:
        """
        Highest pending sell-stop price (the next to trigger as the market falls), or 0 if none.
        """
    def on_trade(self, trade_price: typing.SupportsInt | typing.SupportsIndex, timestamp: typing.SupportsInt | typing.SupportsIndex) -> None:
        """
        record_trade() + drain_triggers(), for callers that are not inside a book callback.
        """
    def pending_stop_count(self) -> int:
        ...
    def record_trade(self, trade_price: typing.SupportsInt | typing.SupportsIndex, timestamp: typing.SupportsInt | typing.SupportsIndex) -> None:
        """
        Note a trade for trigger evaluation without submitting anything. Safe to call from an on_fill callback, where the book is mid-operation.
        """
    def submit(self, order: Order, on_ack: collections.abc.Callable[[AckEvent], None] = None) -> None:
        """
        Accept a STOP or STOP_LIMIT order. Acknowledged with PENDING_TRIGGER if held, REJECTED otherwise.
        """
class SimulationConfig:
    emit_on_every_order: bool
    symbol: str
    verbose: bool
    def __init__(self) -> None:
        ...
    @property
    def end_time(self) -> int:
        ...
    @end_time.setter
    def end_time(self, arg0: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    @property
    def order_id_start(self) -> int:
        ...
    @order_id_start.setter
    def order_id_start(self, arg0: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    @property
    def snapshot_depth(self) -> int:
        ...
    @snapshot_depth.setter
    def snapshot_depth(self, arg0: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    @property
    def start_time(self) -> int:
        ...
    @start_time.setter
    def start_time(self, arg0: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    @property
    def yield_every_n_orders(self) -> int:
        ...
    @yield_every_n_orders.setter
    def yield_every_n_orders(self, arg0: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
class ReplaySpeed:
    """
    Members:
    
      FAST
    
      REALTIME
    
      FIXED_NS
    """
    FAST: typing.ClassVar[ReplaySpeed]
    FIXED_NS: typing.ClassVar[ReplaySpeed]
    REALTIME: typing.ClassVar[ReplaySpeed]
    __members__: typing.ClassVar[dict[str, ReplaySpeed]]
    def __eq__(self, other: typing.Any) -> bool:
        ...
    def __getstate__(self) -> int:
        ...
    def __hash__(self) -> int:
        ...
    def __index__(self) -> int:
        ...
    def __init__(self, value: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def __int__(self) -> int:
        ...
    def __ne__(self, other: typing.Any) -> bool:
        ...
    def __repr__(self) -> str:
        ...
    def __setstate__(self, state: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def __str__(self) -> str:
        ...
    @property
    def name(self) -> str:
        ...
    @property
    def value(self) -> int:
        ...
class HistoricalReplayerConfig:
    path: str
    speed: ReplaySpeed
    symbol_filter: str
    verbose: bool
    def __init__(self) -> None:
        ...
    @property
    def end_time(self) -> int:
        ...
    @end_time.setter
    def end_time(self, arg0: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    @property
    def fixed_ns(self) -> int:
        ...
    @fixed_ns.setter
    def fixed_ns(self, arg0: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    @property
    def start_time(self) -> int:
        ...
    @start_time.setter
    def start_time(self, arg0: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
class OrderSource:
    pass
class InteractiveSource(OrderSource):
    def __init__(self, name: str = 'InteractiveSource') -> None:
        ...
    def exhausted(self) -> bool:
        ...
    def is_empty(self) -> bool:
        ...
    def name(self) -> str:
        ...
    def pending_count(self) -> int:
        ...
    def submit(self, order: Order) -> None:
        """
        Queue an order for the engine to process on its next run(). An order with id == 0 is assigned a unique id here.
        """
    def submit_cancel(self, order_id: typing.SupportsInt | typing.SupportsIndex, symbol: str = '', timestamp: typing.SupportsInt | typing.SupportsIndex = 0) -> None:
        """
        Queue a cancellation of a resting order. For a stop that has not triggered yet, use SimulationEngine.stop_manager().cancel() instead — a pending stop is not in the book.
        """
class StrategySource(OrderSource):
    def __init__(self, callback: collections.abc.Callable[[BookSnapshot, typing.SupportsInt | typing.SupportsIndex], list[Order]], name: str = 'StrategySource') -> None:
        """
        callback(snapshot: BookSnapshot, timestamp: int) -> list[Order]. Returned orders must carry a non-zero id — unlike InteractiveSource, this source does not assign one, and the engine's validator rejects id == 0.
        """
    def callback_invocations(self) -> int:
        ...
    def exhausted(self) -> bool:
        ...
    def name(self) -> str:
        ...
    def pending_order_count(self) -> int:
        ...
    def set_callback(self, callback: collections.abc.Callable[[BookSnapshot, typing.SupportsInt | typing.SupportsIndex], list[Order]]) -> None:
        """
        Swap in a new callback, discarding any orders the old one had buffered.
        """
    def total_orders_submitted(self) -> int:
        ...
class HistoricalReplayer(OrderSource):
    def __init__(self, config: HistoricalReplayerConfig) -> None:
        ...
    def bytes_read(self) -> int:
        ...
    def exhausted(self) -> bool:
        ...
    def messages_parsed(self) -> int:
        ...
    def name(self) -> str:
        ...
    def orders_replayed(self) -> int:
        ...
    def orders_skipped(self) -> int:
        ...
class SimulationEngine:
    def __init__(self, config: SimulationConfig, book: OrderBookInterface) -> None:
        ...
    def add_source(self, source: OrderSource) -> None:
        ...
    def book(self) -> OrderBookInterface:
        ...
    def config(self) -> SimulationConfig:
        ...
    def current_time(self) -> int:
        ...
    def heap_size(self) -> int:
        """
        Diagnostic: entries in the source-merge heap. Stays O(sources) however long the engine runs.
        """
    def historical_replayer(self) -> HistoricalReplayer:
        ...
    def interactive_source(self) -> InteractiveSource:
        ...
    def is_running(self) -> bool:
        ...
    def orders_processed(self) -> int:
        ...
    def orders_rejected(self) -> int:
        ...
    def reset(self) -> None:
        ...
    def run(self) -> None:
        ...
    def set_ack_callback(self, callback: collections.abc.Callable[[AckEvent], None]) -> None:
        ...
    def set_cancel_callback(self, callback: collections.abc.Callable[[CancelEvent], None]) -> None:
        ...
    def set_fill_callback(self, callback: collections.abc.Callable[[FillEvent], None]) -> None:
        ...
    def set_snapshot_callback(self, callback: collections.abc.Callable[[BookSnapshot], None]) -> None:
        ...
    def set_tick_callback(self, callback: collections.abc.Callable[[typing.SupportsInt | typing.SupportsIndex, BookSnapshot], None]) -> None:
        ...
    def set_trigger_callback(self, callback: collections.abc.Callable[[TriggerEvent], None]) -> None:
        ...
    def stop(self) -> None:
        ...
    def stop_manager(self) -> StopOrderManager:
        """
        The engine's stop-order manager — where pending STOP and STOP_LIMIT orders live. Query or cancel them here; they are not in the book.
        """
    def stops_triggered(self) -> int:
        ...
    def strategy_source(self) -> StrategySource:
        ...
    def tick_count(self) -> int:
        ...
def format_price(basis_points: typing.SupportsInt | typing.SupportsIndex) -> str:
    """
    Format a basis-point price as a human-readable string.
    """
def from_basis_points(basis_points: typing.SupportsInt | typing.SupportsIndex) -> float:
    """
    Convert basis points back to a double price.
    """
def opposite(side: Side) -> Side:
    """
    Return the opposite side (BID <-> ASK).
    """
def tick_ceil(price: typing.SupportsInt | typing.SupportsIndex, tick_size: typing.SupportsInt | typing.SupportsIndex) -> int:
    ...
def tick_floor(price: typing.SupportsInt | typing.SupportsIndex, tick_size: typing.SupportsInt | typing.SupportsIndex) -> int:
    ...
def to_basis_points(price: typing.SupportsFloat | typing.SupportsIndex) -> int:
    """
    Convert a double price to basis points (int64), rounded.
    """
BASIS_POINTS_PER_UNIT: int = 10000
