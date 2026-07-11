from typing import Literal, Optional

from pydantic import BaseModel, Field


# ── Inbound requests ──────────────────────────────────────────────────────────

class OrderRequest(BaseModel):
    side: Literal["buy", "sell"]
    price: float = Field(..., gt=0)
    quantity: int = Field(..., gt=0)
    order_type: Literal["limit", "market", "ioc", "fok"] = "limit"
    symbol: str = "AAPL"


class CancelRequest(BaseModel):
    order_id: int


class BacktestRequest(BaseModel):
    symbol: str = "AAPL"
    start_time: str = "2024-01-01T00:00:00Z"
    end_time: str = "2024-01-02T00:00:00Z"
    depth: int = Field(10, ge=1, le=50)


# ── Outbound responses ────────────────────────────────────────────────────────

class BookLevel(BaseModel):
    price: float
    quantity: int
    order_count: int
    order_ids: list[int]          # so the UI can show which orders sit here
    created_at_by_id: dict[int, str] = Field(default_factory=dict)


class RestingOrder(BaseModel):
    order_id: int
    side: Literal["buy", "sell"]
    price: float
    quantity: int
    orig_quantity: int
    order_type: str
    symbol: str
    created_at: str


class FillRecord(BaseModel):
    sequence: int
    aggressive_order_id: int
    passive_order_id: int
    side: Literal["buy", "sell"]   # aggressor side
    price: float
    quantity: int
    symbol: str


class BookSnapshot(BaseModel):
    symbol: str
    bids: list[BookLevel]
    asks: list[BookLevel]
    best_bid: Optional[float]
    best_ask: Optional[float]
    last_trade_price: Optional[float]
    sequence: int
    resting_orders: list[RestingOrder]   # full flat list — UI renders the panels


class OrderSubmissionResponse(BaseModel):
    accepted: bool
    order_id: int
    status: str
    message: str


class CancelResponse(BaseModel):
    accepted: bool
    order_id: int
    message: str


class BacktestRun(BaseModel):
    run_id: str
    status: str
    symbol: str
    start_time: str
    end_time: str
    depth: int
    message: str