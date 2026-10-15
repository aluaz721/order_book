from contextlib import asynccontextmanager
from typing import Optional

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
    ReplayRequest,
    ReplayStatus,
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


# ── Historical replay ─────────────────────────────────────────────────────────
#
# Drives the dashboard from a recorded ITCH 5.0 file. The replay runs on its
# own book, so a user's resting orders survive it untouched, and streams
# through the same /ws/book channel as live trading.

@app.get("/replay/files", response_model=list[str])
def list_replay_files() -> list[str]:
    return bridge.available_replays()


@app.post("/replay/start", response_model=ReplayStatus)
def start_replay(request: ReplayRequest) -> ReplayStatus:
    return bridge.start_replay(request)


@app.post("/replay/stop", response_model=ReplayStatus)
def stop_replay() -> ReplayStatus:
    return bridge.stop_replay()


@app.get("/replay/status", response_model=Optional[ReplayStatus])
def replay_status() -> Optional[ReplayStatus]:
    return bridge.replay_status()


# ── WebSocket ─────────────────────────────────────────────────────────────────

@app.websocket("/ws/book")
async def websocket_book(websocket: WebSocket) -> None:
    await bridge.connect(websocket)