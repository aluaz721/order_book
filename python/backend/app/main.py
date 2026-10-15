from contextlib import asynccontextmanager

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


@asynccontextmanager
async def lifespan(app: FastAPI):
    # Hand the bridge the loop uvicorn serves on. Broadcasts raised from a
    # threadpool worker or the engine thread are marshalled back onto it —
    # see EngineBridge._schedule_broadcast.
    bridge.bind_loop()
    yield


app = FastAPI(
    lifespan=lifespan,
    title="Order Book Dashboard API",
    version="0.3.0",
    description=(
        "REST + WebSocket surface over the C++ limit order book engine. "
        "Matching is performed by TreeOrderBook from the compiled "
        "`order_book` extension; this service only translates and streams."
    ),
)

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


@app.get("/diagnostics")
def diagnostics() -> dict[str, int]:
    """Engine counters vs. the API layer's projection of it.

    `projection_repairs` should always be 0 — a non-zero value means the
    bridge's view of resting orders drifted from the engine's.
    """
    return bridge.diagnostics()


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