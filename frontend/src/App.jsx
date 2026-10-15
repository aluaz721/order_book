import { useCallback, useEffect, useMemo, useRef, useState } from 'react';

// In dev (docker-compose), leave VITE_API_BASE unset and requests go through
// the Vite proxy to the `backend` container. In prod, set VITE_API_BASE to
// the deployed backend's origin (e.g. https://order-book-backend.onrender.com).
const API_BASE = import.meta.env.VITE_API_BASE ?? '';
const WS_PATH = '/ws/book';

// ── Helpers ───────────────────────────────────────────────────────────────────

const fmt = (price) =>
  price == null ? '—' : `$${Number(price).toFixed(2)}`;

const fmtTime = () => {
  const d = new Date();
  return d.toLocaleTimeString('en-US', { hour12: false }) +
    '.' + String(d.getMilliseconds()).padStart(3, '0');
};

// ── useWebSocket hook ─────────────────────────────────────────────────────────

function useOrderBookSocket() {
  const [book, setBook]     = useState(null);
  const [fills, setFills]   = useState([]);
  const [conn, setConn]     = useState('Connecting…');
  const [replay, setReplay] = useState(null);
  const wsRef = useRef(null);

  useEffect(() => {
    const wsBase = API_BASE
      ? API_BASE.replace(/^http/, 'ws')
      : `${location.protocol === 'https:' ? 'wss' : 'ws'}://${location.host}`;
    const url = `${wsBase}${WS_PATH}`;

    const connect = () => {
      const ws = new WebSocket(url);
      wsRef.current = ws;

      ws.onopen    = () => setConn('Live');
      ws.onclose   = () => { setConn('Reconnecting…'); setTimeout(connect, 2000); };
      ws.onerror   = () => setConn('Error');
      ws.onmessage = (ev) => {
        const msg = JSON.parse(ev.data);
        if (msg.type === 'snapshot') {
          setBook(msg.book);
          if (msg.fills) setFills(msg.fills.slice().reverse());
          // Present on every frame: an object while a replay exists (in any
          // state), null once the server has none. Driving straight off it
          // keeps the banner in step even if a replay ends on its own.
          setReplay(msg.replay ?? null);
        }
      };
    };

    connect();
    return () => { wsRef.current?.close(); };
  }, []);

  return { book, fills, conn, replay };
}

// ── OrderForm ─────────────────────────────────────────────────────────────────

const DEFAULT_FORM = { side: 'buy', price: '100.25', quantity: '10',
                       order_type: 'limit', symbol: 'AAPL',
                       // Stop types only. Defaults sit on the sell side of
                       // the book's opening price: a sell stop has to trigger
                       // below the market, and its limit at or above the
                       // trigger (see order_validators.hpp).
                       stop_price: '99.00', limit_price: '99.50' };

const STOP_TYPES = ['stop', 'stop_limit'];

function OrderForm({ onSubmitted }) {
  const [form, setForm]     = useState(DEFAULT_FORM);
  const [busy, setBusy]     = useState(false);
  const [flash, setFlash]   = useState(null);

  const set = (key) => (ev) => setForm((f) => ({ ...f, [key]: ev.target.value }));

  const submit = async (ev) => {
    ev.preventDefault();
    setBusy(true);
    setFlash(null);
    try {
      // Built field by field rather than spread, so a stop price never rides
      // along on a limit order (or vice versa).
      const payload = {
        side: form.side,
        symbol: form.symbol,
        order_type: form.order_type,
        quantity: parseInt(form.quantity, 10),
        price: parseFloat(isStop ? form.stop_price : form.price),
      };
      if (isStop) payload.stop_price = parseFloat(form.stop_price);
      if (form.order_type === 'stop_limit') {
        payload.limit_price = parseFloat(form.limit_price);
      }

      const res = await fetch(`${API_BASE}/orders`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(payload),
      });
      const data = await res.json();
      setFlash({ ok: data.accepted, msg: data.message });
      if (onSubmitted) onSubmitted();
    } catch {
      setFlash({ ok: false, msg: 'Network error.' });
    } finally {
      setBusy(false);
    }
  };

  const isStop = STOP_TYPES.includes(form.order_type);
  const needsPrice = form.order_type !== 'market';

  return (
    <form onSubmit={submit} className="order-form">
      <div className="form-row">
        <label className="field">
          <span>Side</span>
          <select value={form.side} onChange={set('side')}
                  className={form.side === 'buy' ? 'select-bid' : 'select-ask'}>
            <option value="buy">Buy</option>
            <option value="sell">Sell</option>
          </select>
        </label>

        <label className="field">
          <span>Type</span>
          <select value={form.order_type} onChange={set('order_type')}>
            <option value="limit">Limit</option>
            <option value="market">Market</option>
            <option value="market_limit">Market Limit</option>
            <option value="ioc">IOC</option>
            <option value="fok">FOK</option>
            <option value="stop">Stop</option>
            <option value="stop_limit">Stop Limit</option>
          </select>
        </label>

        {/* For a stop, the engine's price field IS the trigger price, so the
            box is relabelled rather than shown alongside an unused one. */}
        <label className="field">
          <span>{isStop ? 'Stop price' : 'Price'}</span>
          <input
            type="number" step="0.01" min="0.01"
            value={isStop ? form.stop_price : form.price}
            onChange={set(isStop ? 'stop_price' : 'price')}
            disabled={!needsPrice}
            className={!needsPrice ? 'disabled' : ''}
          />
        </label>

        {form.order_type === 'stop_limit' && (
          <label className="field">
            <span>Limit price</span>
            <input
              type="number" step="0.01" min="0.01"
              value={form.limit_price} onChange={set('limit_price')}
            />
          </label>
        )}

        <label className="field">
          <span>Quantity</span>
          <input type="number" step="1" min="1"
                 value={form.quantity} onChange={set('quantity')} />
        </label>

        <label className="field">
          <span>Symbol</span>
          <input value={form.symbol} onChange={set('symbol')} />
        </label>

        <button type="submit" className={`btn-submit ${form.side}`}
                disabled={busy}>
          {busy ? '…' : `${form.side === 'buy' ? 'Buy' : 'Sell'}`}
        </button>
      </div>

      {flash && (
        <p className={`flash ${flash.ok ? 'flash-ok' : 'flash-err'}`}>
          {flash.msg}
        </p>
      )}
    </form>
  );
}

// ── BookPanel ─────────────────────────────────────────────────────────────────

function BookPanel({ side, levels = [], onCancel }) {
  const isBid   = side === 'bid';
  const sorted  = useMemo(() =>
    [...levels].sort((a, b) => isBid ? b.price - a.price : a.price - b.price),
    [levels, isBid]
  );

  const maxQty = useMemo(
    () => Math.max(...sorted.map((l) => l.quantity), 1),
    [sorted]
  );

  return (
    <div className={`book-panel ${side}`}>
      <div className="book-header">
        <span>Time</span>
        <span>Order ID</span>
        <span>Qty</span>
        <span>{isBid ? 'BID' : 'ASK'}</span>
        <span>Depth</span>
        <span></span>
      </div>

      {sorted.length === 0 && (
        <div className="book-empty">No resting {isBid ? 'bids' : 'asks'}</div>
      )}

      {sorted.map((level) =>
        level.order_ids.map((oid) => {
          const pct = Math.round((level.quantity / maxQty) * 100);
          const rowMeta = level.created_at_by_id?.[oid] ?? null;
          return (
            <div key={oid} className={`book-row ${side}`}>
              {/* depth bar behind the row */}
              <div
                className={`depth-bar ${side}`}
                style={{ width: `${pct}%`, [isBid ? 'right' : 'left']: 0 }}
              />
              <span className="col-time">{rowMeta ?? '—'}</span>
              <span className="col-id">{oid}</span>
              <span className="col-qty">{level.quantity}</span>
              <span className={`col-price ${side}`}>{fmt(level.price)}</span>
              <span className="col-depth">{pct}%</span>
              <span className="col-cancel">
                <button className="btn-cancel" onClick={() => onCancel(oid)}
                        title={`Cancel order ${oid}`}>✕</button>
              </span>
            </div>
          );
        })
      )}
    </div>
  );
}

// ── PendingStops ──────────────────────────────────────────────────────────────
//
// Stops get their own panel because they are deliberately absent from the
// book: the engine holds them in a StopOrderManager until a trade crosses
// their price, so they have no level, no queue position and no effect on
// depth. Without somewhere to show them, an accepted stop would look like an
// order that disappeared.

function PendingStops({ stops = [], onCancel }) {
  if (stops.length === 0) {
    return (
      <div className="book-empty">
        No pending stops — a stop order waits off the book until a trade
        crosses its price, then enters as a market or limit order.
      </div>
    );
  }

  return (
    <div className="fill-table-wrap">
      <table className="fill-table">
        <thead>
          <tr>
            <th>Time</th>
            <th>Order ID</th>
            <th>Side</th>
            <th>Type</th>
            <th>Trigger</th>
            <th>Limit</th>
            <th>Qty</th>
            <th></th>
          </tr>
        </thead>
        <tbody>
          {stops.map((s) => (
            <tr key={s.order_id} className={`fill-row ${s.side}`}>
              <td>{s.created_at}</td>
              <td>{s.order_id}</td>
              <td className={s.side}>{s.side.toUpperCase()}</td>
              <td>{s.order_type === 'stop_limit' ? 'STOP LIMIT' : 'STOP'}</td>
              <td>{fmt(s.stop_price)}</td>
              <td>{s.limit_price == null ? '—' : fmt(s.limit_price)}</td>
              <td>{s.quantity}</td>
              <td>
                <button className="btn-cancel" onClick={() => onCancel(s.order_id)}
                        title={`Cancel pending stop ${s.order_id}`}>✕</button>
              </td>
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  );
}

// ── FillTable ─────────────────────────────────────────────────────────────────

function FillTable({ fills }) {
  return (
    <div className="fill-table-wrap">
      <table className="fill-table">
        <thead>
          <tr>
            <th>#</th>
            <th>Side</th>
            <th>Price</th>
            <th>Qty</th>
            <th>Aggressor</th>
            <th>Passive</th>
            <th>Symbol</th>
          </tr>
        </thead>
        <tbody>
          {fills.length === 0 && (
            <tr><td colSpan={7} className="fill-empty">No matches yet</td></tr>
          )}
          {fills.map((f) => (
            <tr key={f.sequence} className={`fill-row ${f.side}`}>
              <td>{f.sequence}</td>
              <td className={f.side}>{f.side.toUpperCase()}</td>
              <td>{fmt(f.price)}</td>
              <td>{f.quantity}</td>
              <td>{f.aggressive_order_id}</td>
              <td>{f.passive_order_id}</td>
              <td>{f.symbol}</td>
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  );
}

// ── ReplayPanel ───────────────────────────────────────────────────────────────
//
// Replay streams through the same /ws/book channel as live trading, so the
// book and fill panels need no replay-specific code — they just render the
// snapshots that arrive. This panel owns the controls and makes it obvious
// that what is on screen is recorded, not live.

const SPEED_OPTIONS = [
  { value: 'fixed',    label: 'Steady',    hint: 'Even gap between events' },
  { value: 'realtime', label: 'Real time', hint: "The file's own timing" },
  { value: 'fast',     label: 'As fast as possible', hint: 'No pacing at all' },
];

const PACE_OPTIONS = [
  { value: 100_000_000, label: '0.1s' },
  { value: 40_000_000,  label: '40ms' },
  { value: 10_000_000,  label: '10ms' },
  { value: 2_000_000,   label: '2ms' },
];

function ReplayPanel({ replay }) {
  const [files, setFiles]   = useState([]);
  const [file, setFile]     = useState('');
  const [speed, setSpeed]   = useState('fixed');
  const [pace, setPace]     = useState(40_000_000);
  const [busy, setBusy]     = useState(false);
  const [error, setError]   = useState(null);

  useEffect(() => {
    let cancelled = false;
    fetch(`${API_BASE}/replay/files`)
      .then((r) => (r.ok ? r.json() : []))
      .then((names) => {
        if (cancelled) return;
        setFiles(names);
        setFile((current) => current || names[0] || '');
      })
      .catch(() => { if (!cancelled) setFiles([]); });
    return () => { cancelled = true; };
  }, []);

  const running = replay?.state === 'running' || replay?.state === 'starting';

  const start = async () => {
    setBusy(true);
    setError(null);
    try {
      const res = await fetch(`${API_BASE}/replay/start`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ path: file, symbol: 'AAPL',
                               speed, fixed_ns: pace }),
      });
      if (!res.ok) {
        const body = await res.json().catch(() => ({}));
        setError(body.detail ?? `Could not start replay (${res.status})`);
      }
    } catch (err) {
      setError(String(err));
    } finally {
      setBusy(false);
    }
  };

  const stop = async () => {
    setBusy(true);
    setError(null);
    try {
      await fetch(`${API_BASE}/replay/stop`, { method: 'POST' });
    } catch (err) {
      setError(String(err));
    } finally {
      setBusy(false);
    }
  };

  const total    = replay?.events_total ?? 0;
  const done     = replay?.events_replayed ?? 0;
  const progress = total > 0 ? Math.min(100, (done / total) * 100) : 0;

  if (files.length === 0 && !replay) {
    return (
      <div className="book-empty">
        No replay files on the server. Generate one with
        {' '}<code>python3 scripts/generate_itch.py</code>.
      </div>
    );
  }

  return (
    <div className="replay-panel">
      <div className="form-row">
        <label className="field">
          <span>File</span>
          <select value={file} disabled={running}
                  onChange={(e) => setFile(e.target.value)}>
            {files.map((name) => <option key={name} value={name}>{name}</option>)}
          </select>
        </label>

        <label className="field">
          <span>Pacing</span>
          <select value={speed} disabled={running}
                  onChange={(e) => setSpeed(e.target.value)}>
            {SPEED_OPTIONS.map((o) => (
              <option key={o.value} value={o.value}>{o.label}</option>
            ))}
          </select>
        </label>

        <label className="field">
          <span>Gap</span>
          <select value={pace} disabled={running || speed !== 'fixed'}
                  onChange={(e) => setPace(Number(e.target.value))}>
            {PACE_OPTIONS.map((o) => (
              <option key={o.value} value={o.value}>{o.label}</option>
            ))}
          </select>
        </label>

        {running ? (
          <button type="button" className="btn-submit sell"
                  onClick={stop} disabled={busy}>
            Stop replay
          </button>
        ) : (
          <button type="button" className="btn-submit buy"
                  onClick={start} disabled={busy || !file}>
            Start replay
          </button>
        )}
      </div>

      {speed === 'realtime' && !running && (
        <p className="replay-note">
          Real time reproduces the recorded gaps exactly, including quiet
          stretches — the book may sit still for a while.
        </p>
      )}

      {error && <p className="replay-error">{error}</p>}

      {replay && (
        <div className="replay-status">
          <div className="replay-progress">
            <div className="replay-progress-fill"
                 style={{ width: `${progress}%` }} />
          </div>
          <div className="replay-meta">
            <span className={`replay-state replay-state-${replay.state}`}>
              {replay.state}
            </span>
            <span>{done.toLocaleString()} / {total.toLocaleString()} events</span>
            <span>{replay.fills} fills</span>
            <span>{replay.elapsed_seconds}s elapsed</span>
          </div>
          {replay.error && <p className="replay-error">{replay.error}</p>}
        </div>
      )}
    </div>
  );
}

// ── StatsBar ──────────────────────────────────────────────────────────────────

function StatsBar({ book, conn }) {
  return (
    <div className="stats-bar">
      <div className="stat">
        <span className="stat-label">Best Bid</span>
        <span className="stat-value bid">{fmt(book?.best_bid)}</span>
      </div>
      <div className="stat-divider" />
      <div className="stat">
        <span className="stat-label">Last Trade</span>
        <span className="stat-value">{fmt(book?.last_trade_price)}</span>
      </div>
      <div className="stat-divider" />
      <div className="stat">
        <span className="stat-label">Best Ask</span>
        <span className="stat-value ask">{fmt(book?.best_ask)}</span>
      </div>
      <div className="stat-divider" />
      <div className="stat">
        <span className="stat-label">Spread</span>
        <span className="stat-value">
          {book?.best_bid && book?.best_ask
            ? fmt(book.best_ask - book.best_bid)
            : '—'}
        </span>
      </div>
      <div className="stat-spacer" />
      <div className="stat">
        <span className="stat-label">Stream</span>
        <span className={`stat-value conn-${conn === 'Live' ? 'live' : 'dead'}`}>
          {conn}
        </span>
      </div>
      <div className="stat">
        <span className="stat-label">Seq</span>
        <span className="stat-value mono">{book?.sequence ?? '—'}</span>
      </div>
    </div>
  );
}

// ── App ───────────────────────────────────────────────────────────────────────

export default function App() {
  const { book, fills, conn, replay } = useOrderBookSocket();

  const cancelOrder = useCallback(async (orderId) => {
    await fetch(`${API_BASE}/orders/${orderId}`, { method: 'DELETE' });
    // snapshot will arrive via WebSocket — no manual refresh needed
  }, []);

  const replayActive = replay?.state === 'running' || replay?.state === 'starting';

  const bids = book?.bids ?? [];
  const asks = book?.asks ?? [];
  const pendingStops = book?.pending_stops ?? [];

  return (
    <div className="shell">

      {/* ── Header ── */}
      <header className="top-bar">
        <div className="top-bar-left">
          <span className="logo">My Limit Order Book</span>
        </div>
        <div className="top-bar-right">
          <span className="symbol-badge">{book?.symbol ?? 'AAPL'}</span>
        </div>
      </header>

      {/* ── Replay banner ── */}
      {replayActive && (
        <div className="replay-banner">
          Replaying <strong>{replay.path.split('/').pop()}</strong> — the book
          below is recorded market data, not live trading.
        </div>
      )}

      {/* ── Stats ── */}
      <StatsBar book={book} conn={conn} />

      {/* ── Historical replay ── */}
      <section className="section">
        <h2 className="section-title">Historical Replay</h2>
        <ReplayPanel replay={replay} />
      </section>

      {/* ── Order injection ── */}
      <section className="section">
        <h2 className="section-title">Inject Order</h2>
        {replayActive ? (
          <div className="book-empty">
            Paused while a replay is running. Your resting orders are
            untouched — the replay runs on its own book — and the form
            returns when it stops.
          </div>
        ) : (
          <OrderForm />
        )}
      </section>

      {/* ── Book panels ── */}
      <section className="section">
        <h2 className="section-title">Order Book</h2>
        <div className="book-grid">
          <div>
            <div className="panel-label bid">BUY ORDERS</div>
            <BookPanel side="bid" levels={bids} onCancel={cancelOrder} />
          </div>
          <div>
            <div className="panel-label ask">SELL ORDERS</div>
            <BookPanel side="ask" levels={asks} onCancel={cancelOrder} />
          </div>
        </div>
      </section>

      {/* ── Pending stops ── */}
      <section className="section">
        <h2 className="section-title">
          Pending Stops
          <span className="fill-count">{pendingStops.length} waiting</span>
        </h2>
        <PendingStops stops={pendingStops} onCancel={cancelOrder} />
      </section>

      {/* ── Fill history ── */}
      <section className="section">
        <h2 className="section-title">
          Match History
          <span className="fill-count">{fills.length} fills</span>
        </h2>
        <FillTable fills={fills} />
      </section>

    </div>
  );
}