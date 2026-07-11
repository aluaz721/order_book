from fastapi import FastAPI, WebSocket
from fastapi.middleware.cors import CORSMiddleware

from .models import (
    BacktestRequest,
    BacktestRun,
    CancelRequest,
    CancelResponse,
    FillRecord,
    OrderRequest,
    OrderSubmissionResponse,
)
from .services import EngineBridge

app = FastAPI(title="Order Book Dashboard API", version="0.2.0")

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

bridge = EngineBridge()


@app.get("/health")
def health() -> dict[str, str]:
    return {"status": "ok"}


# ── Orders ────────────────────────────────────────────────────────────────────

@app.post("/orders", response_model=OrderSubmissionResponse)
def submit_order(request: OrderRequest) -> OrderSubmissionResponse:
    return bridge.submit_order(request)


@app.delete("/orders/{order_id}", response_model=CancelResponse)
def cancel_order(order_id: int) -> CancelResponse:
    return bridge.cancel_order(CancelRequest(order_id=order_id))


# ── Fills ─────────────────────────────────────────────────────────────────────

@app.get("/fills", response_model=list[FillRecord])
def get_fills(limit: int = 50) -> list[FillRecord]:
    return bridge.get_fills(limit)


# ── Backtests ─────────────────────────────────────────────────────────────────

@app.post("/backtests", response_model=BacktestRun)
def create_backtest(request: BacktestRequest) -> BacktestRun:
    return bridge.start_backtest(request)


@app.get("/backtests/{run_id}", response_model=BacktestRun)
def get_backtest(run_id: str) -> BacktestRun:
    return bridge.get_backtest(run_id)


# ── WebSocket ─────────────────────────────────────────────────────────────────

@app.websocket("/ws/book")
async def websocket_book(websocket: WebSocket) -> None:
    await bridge.connect(websocket)