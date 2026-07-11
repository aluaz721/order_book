import { useCallback, useEffect, useMemo, useRef, useState } from 'react';

const API = '/api';
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
  const [book, setBook]   = useState(null);
  const [fills, setFills] = useState([]);
  const [conn, setConn]   = useState('Connecting…');
  const wsRef = useRef(null);

  useEffect(() => {
    const proto = location.protocol === 'https:' ? 'wss' : 'ws';
    const url   = `${proto}://${location.host}${WS_PATH}`;

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
        }
      };
    };

    connect();
    return () => { wsRef.current?.close(); };
  }, []);

  return { book, fills, conn };
}

// ── OrderForm ─────────────────────────────────────────────────────────────────

const DEFAULT_FORM = { side: 'buy', price: '100.25', quantity: '10',
                       order_type: 'limit', symbol: 'AAPL' };

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
      const res = await fetch(`${API}/orders`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          ...form,
          price: parseFloat(form.price),
          quantity: parseInt(form.quantity, 10),
        }),
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
            <option value="ioc">IOC</option>
            <option value="fok">FOK</option>
          </select>
        </label>

        <label className="field">
          <span>Price</span>
          <input
            type="number" step="0.01" min="0.01"
            value={form.price} onChange={set('price')}
            disabled={!needsPrice}
            className={!needsPrice ? 'disabled' : ''}
          />
        </label>

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
  const { book, fills, conn } = useOrderBookSocket();

  const cancelOrder = useCallback(async (orderId) => {
    await fetch(`${API}/orders/${orderId}`, { method: 'DELETE' });
    // snapshot will arrive via WebSocket — no manual refresh needed
  }, []);

  const bids = book?.bids ?? [];
  const asks = book?.asks ?? [];

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

      {/* ── Stats ── */}
      <StatsBar book={book} conn={conn} />

      {/* ── Order injection ── */}
      <section className="section">
        <h2 className="section-title">Inject Order</h2>
        <OrderForm />
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