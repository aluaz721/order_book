#include <pybind11/functional.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "order_book/book/order_book_interface.hpp"
#include "order_book/book/tree_order_book.hpp"
#include "order_book/core/event.hpp"
#include "order_book/core/order.hpp"
#include "order_book/core/price_utils.hpp"
#include "order_book/engine/simulation_config.hpp"
#include "order_book/engine/simulation_engine.hpp"
#include "order_book/matching/fifo_matcher.hpp"
#include "order_book/matching/matching_algorithm.hpp"
#include "order_book/sources/historical_replayer.hpp"
#include "order_book/sources/order_source.hpp"

#include <memory>
#include <string>

namespace py = pybind11;
using namespace order_book;

// Compiled as the private `_order_book` submodule of the `order_book`
// Python package (see python/order_book/__init__.py, which re-exports
// everything from here at the top level) — not imported directly by users.
PYBIND11_MODULE(_order_book, m) {
    m.doc() = "Python bindings for the C++ limit order book engine";

    // ── Enums ────────────────────────────────────────────────────────────────

    py::enum_<Side>(m, "Side")
        .value("BID", Side::BID)
        .value("ASK", Side::ASK);

    py::enum_<OrderType>(m, "OrderType")
        .value("LIMIT", OrderType::LIMIT)
        .value("MARKET", OrderType::MARKET)
        .value("MARKET_LIMIT", OrderType::MARKET_LIMIT)
        .value("STOP", OrderType::STOP)
        .value("STOP_LIMIT", OrderType::STOP_LIMIT)
        .value("IMMEDIATE_OR_CANCEL", OrderType::IMMEDIATE_OR_CANCEL)
        .value("FILL_OR_KILL", OrderType::FILL_OR_KILL);

    py::enum_<OrderStatus>(m, "OrderStatus")
        .value("NEW", OrderStatus::NEW)
        .value("PENDING_TRIGGER", OrderStatus::PENDING_TRIGGER)
        .value("PARTIALLY_FILLED", OrderStatus::PARTIALLY_FILLED)
        .value("FILLED", OrderStatus::FILLED)
        .value("CANCELLED", OrderStatus::CANCELLED)
        .value("REJECTED", OrderStatus::REJECTED);

    py::enum_<CancelReason>(m, "CancelReason")
        .value("CLIENT_REQUEST", CancelReason::CLIENT_REQUEST)
        .value("IOC_EXPIRED", CancelReason::IOC_EXPIRED)
        .value("FOK_FAILED", CancelReason::FOK_FAILED)
        .value("MARKET_NO_LIQUIDITY", CancelReason::MARKET_NO_LIQUIDITY)
        .value("RISK_REJECTED", CancelReason::RISK_REJECTED)
        .value("STOP_CANCELLED", CancelReason::STOP_CANCELLED)
        .value("SELF_TRADE", CancelReason::SELF_TRADE);

    m.def("opposite", &opposite, py::arg("side"),
          "Return the opposite side (BID <-> ASK).");

    // ── Order ────────────────────────────────────────────────────────────────

    py::class_<Order>(m, "Order")
        .def(py::init([](uint64_t id, std::string symbol, Side side, OrderType type,
                          int64_t price, uint64_t quantity, uint64_t timestamp,
                          int64_t limit_price, OrderStatus status) {
                 Order o{};
                 o.id                 = id;
                 o.symbol             = std::move(symbol);
                 o.side               = side;
                 o.type               = type;
                 o.price              = price;
                 o.limit_price        = limit_price;
                 o.quantity_remaining = quantity;
                 o.orig_quantity      = quantity;
                 o.timestamp          = timestamp;
                 o.status             = status;
                 return o;
             }),
             py::arg("id"), py::arg("symbol"), py::arg("side"), py::arg("type"),
             py::arg("price"), py::arg("quantity"), py::arg("timestamp") = 0,
             py::arg("limit_price") = 0, py::arg("status") = OrderStatus::NEW)
        .def_readwrite("id", &Order::id)
        .def_readwrite("symbol", &Order::symbol)
        .def_readwrite("side", &Order::side)
        .def_readwrite("type", &Order::type)
        .def_readwrite("price", &Order::price)
        .def_readwrite("limit_price", &Order::limit_price)
        .def_readwrite("quantity_remaining", &Order::quantity_remaining)
        .def_readwrite("orig_quantity", &Order::orig_quantity)
        .def_readwrite("timestamp", &Order::timestamp)
        .def_readwrite("status", &Order::status)
        .def("__repr__", [](const Order& o) {
            return "<Order id=" + std::to_string(o.id) +
                   " symbol=" + o.symbol +
                   " side=" + to_string(o.side) +
                   " type=" + to_string(o.type) +
                   " price=" + std::to_string(o.price) +
                   " qty_remaining=" + std::to_string(o.quantity_remaining) + ">";
        });

    // ── Events / snapshots ───────────────────────────────────────────────────

    py::class_<FillEvent>(m, "FillEvent")
        .def_readonly("aggressive_order_id", &FillEvent::aggressive_order_id)
        .def_readonly("passive_order_id", &FillEvent::passive_order_id)
        .def_readonly("symbol", &FillEvent::symbol)
        .def_readonly("aggressor_side", &FillEvent::aggressor_side)
        .def_readonly("fill_price", &FillEvent::fill_price)
        .def_readonly("fill_quantity", &FillEvent::fill_quantity)
        .def_readonly("timestamp", &FillEvent::timestamp)
        .def_readonly("sequence", &FillEvent::sequence);

    py::class_<CancelEvent>(m, "CancelEvent")
        .def_readonly("order_id", &CancelEvent::order_id)
        .def_readonly("symbol", &CancelEvent::symbol)
        .def_readonly("side", &CancelEvent::side)
        .def_readonly("price", &CancelEvent::price)
        .def_readonly("reason", &CancelEvent::reason)
        .def_readonly("remaining_quantity", &CancelEvent::remaining_quantity)
        .def_readonly("timestamp", &CancelEvent::timestamp)
        .def_readonly("sequence", &CancelEvent::sequence);

    py::class_<TriggerEvent>(m, "TriggerEvent")
        .def_readonly("stop_order_id", &TriggerEvent::stop_order_id)
        .def_readonly("triggered_order_id", &TriggerEvent::triggered_order_id)
        .def_readonly("symbol", &TriggerEvent::symbol)
        .def_readonly("side", &TriggerEvent::side)
        .def_readonly("stop_price", &TriggerEvent::stop_price)
        .def_readonly("trigger_trade_price", &TriggerEvent::trigger_trade_price)
        .def_readonly("converted_to", &TriggerEvent::converted_to)
        .def_readonly("timestamp", &TriggerEvent::timestamp);

    py::class_<AckEvent>(m, "AckEvent")
        .def_readonly("order_id", &AckEvent::order_id)
        .def_readonly("symbol", &AckEvent::symbol)
        .def_readonly("status", &AckEvent::status)
        .def_readonly("message", &AckEvent::message)
        .def_readonly("timestamp", &AckEvent::timestamp);

    py::class_<BookLevel>(m, "BookLevel")
        .def_readonly("price", &BookLevel::price)
        .def_readonly("quantity", &BookLevel::quantity)
        .def_readonly("order_count", &BookLevel::order_count);

    py::class_<BookSnapshot>(m, "BookSnapshot")
        .def_readonly("symbol", &BookSnapshot::symbol)
        .def_readonly("bids", &BookSnapshot::bids)
        .def_readonly("asks", &BookSnapshot::asks)
        .def_readonly("last_trade_price", &BookSnapshot::last_trade_price)
        .def_readonly("timestamp", &BookSnapshot::timestamp)
        .def_readonly("sequence", &BookSnapshot::sequence);

    py::class_<OrderBookCallbacks>(m, "OrderBookCallbacks")
        .def(py::init<>())
        .def_readwrite("on_fill", &OrderBookCallbacks::on_fill)
        .def_readwrite("on_cancel", &OrderBookCallbacks::on_cancel)
        .def_readwrite("on_ack", &OrderBookCallbacks::on_ack)
        .def_readwrite("on_book_update", &OrderBookCallbacks::on_book_update);

    // ── Matching algorithms ──────────────────────────────────────────────────
    // NOTE: ProRataMatcher is declared in the C++ headers but not yet linked
    // into the static library (no .cpp implementation exists), so it is not
    // bound here. Only FIFOMatcher is currently usable.

    // Bound with the smart_holder so unique_ptr<MatchingAlgorithm> ownership
    // can be transferred from Python into TreeOrderBook's constructor.
    py::class_<MatchingAlgorithm, py::smart_holder>(m, "MatchingAlgorithm");

    py::class_<FIFOMatcher, MatchingAlgorithm, py::smart_holder>(m, "FIFOMatcher")
        .def(py::init<>())
        .def("name", &FIFOMatcher::name);

    // ── TreeOrderBook ─────────────────────────────────────────────────────────
    // Bound with the smart_holder, and OrderBookInterface registered as its
    // base, so a Python-constructed TreeOrderBook can be moved (via
    // unique_ptr<OrderBookInterface>) into SimulationEngine's constructor,
    // and so SimulationEngine::book() — which returns a base-class reference
    // — resolves back to the concrete TreeOrderBook Python wrapper.

    py::class_<OrderBookInterface, py::smart_holder>(m, "OrderBookInterface");

    py::class_<TreeOrderBook, OrderBookInterface, py::smart_holder>(m, "TreeOrderBook")
        .def(py::init<std::string, std::unique_ptr<MatchingAlgorithm>, OrderBookCallbacks>(),
             py::arg("symbol"), py::arg("matcher"),
             py::arg("callbacks") = OrderBookCallbacks{})
        .def("add", &TreeOrderBook::add, py::arg("order"))
        .def("cancel", &TreeOrderBook::cancel, py::arg("order_id"))
        .def("execute", &TreeOrderBook::execute, py::arg("order_id"),
             py::arg("quantity"), py::arg("timestamp"))
        .def("reduce", &TreeOrderBook::reduce, py::arg("order_id"),
             py::arg("quantity"), py::arg("timestamp"))
        .def("replace", &TreeOrderBook::replace, py::arg("old_order_id"),
             py::arg("new_order"))
        .def("best_bid", &TreeOrderBook::best_bid)
        .def("best_ask", &TreeOrderBook::best_ask)
        .def("spread", &TreeOrderBook::spread)
        .def("mid_price", &TreeOrderBook::mid_price)
        .def("weighted_mid", &TreeOrderBook::weighted_mid)
        .def("total_bid_qty", &TreeOrderBook::total_bid_qty)
        .def("total_ask_qty", &TreeOrderBook::total_ask_qty)
        .def("bid_depth", &TreeOrderBook::bid_depth)
        .def("ask_depth", &TreeOrderBook::ask_depth)
        .def("has_order", &TreeOrderBook::has_order, py::arg("order_id"))
        .def("last_trade_price", &TreeOrderBook::last_trade_price)
        .def("snapshot", &TreeOrderBook::snapshot, py::arg("depth") = 10)
        .def("symbol", &TreeOrderBook::symbol)
        .def("sequence", &TreeOrderBook::sequence)
        .def("set_callbacks", &TreeOrderBook::set_callbacks, py::arg("callbacks"))
        .def("set_matching_algorithm", &TreeOrderBook::set_matching_algorithm,
             py::arg("algo"));

    // ── SimulationConfig ─────────────────────────────────────────────────────

    py::class_<SimulationConfig>(m, "SimulationConfig")
        .def(py::init<>())
        .def_readwrite("symbol", &SimulationConfig::symbol)
        .def_readwrite("snapshot_depth", &SimulationConfig::snapshot_depth)
        .def_readwrite("emit_on_every_order", &SimulationConfig::emit_on_every_order)
        .def_readwrite("start_time", &SimulationConfig::start_time)
        .def_readwrite("end_time", &SimulationConfig::end_time)
        .def_readwrite("yield_every_n_orders", &SimulationConfig::yield_every_n_orders)
        .def_readwrite("order_id_start", &SimulationConfig::order_id_start)
        .def_readwrite("verbose", &SimulationConfig::verbose);

    // ── HistoricalReplayer ────────────────────────────────────────────────────
    // NOTE: only handles the flat, length-prefixed ITCH 5.0 framing used by
    // NASDAQ's downloadable sample files — see historical_replayer.hpp for
    // details and the full list of handled/skipped message types.

    py::enum_<ReplaySpeed>(m, "ReplaySpeed")
        .value("FAST", ReplaySpeed::FAST)
        .value("REALTIME", ReplaySpeed::REALTIME)
        .value("FIXED_NS", ReplaySpeed::FIXED_NS);

    py::class_<HistoricalReplayer::Config>(m, "HistoricalReplayerConfig")
        .def(py::init<>())
        .def_readwrite("path", &HistoricalReplayer::Config::path)
        .def_readwrite("symbol_filter", &HistoricalReplayer::Config::symbol_filter)
        .def_readwrite("speed", &HistoricalReplayer::Config::speed)
        .def_readwrite("fixed_ns", &HistoricalReplayer::Config::fixed_ns)
        .def_readwrite("start_time", &HistoricalReplayer::Config::start_time)
        .def_readwrite("end_time", &HistoricalReplayer::Config::end_time)
        .def_readwrite("verbose", &HistoricalReplayer::Config::verbose);

    // OrderSource is bound only as an (empty) base so unique_ptr<OrderSource>
    // ownership transfer into SimulationEngine::add_source() works — Python
    // code never needs to call anything on it directly.
    py::class_<OrderSource, py::smart_holder>(m, "OrderSource");

    py::class_<HistoricalReplayer, OrderSource, py::smart_holder>(m, "HistoricalReplayer")
        .def(py::init<HistoricalReplayer::Config>(), py::arg("config"))
        .def("messages_parsed", &HistoricalReplayer::messages_parsed)
        .def("orders_replayed", &HistoricalReplayer::orders_replayed)
        .def("orders_skipped", &HistoricalReplayer::orders_skipped)
        .def("bytes_read", &HistoricalReplayer::bytes_read)
        .def("exhausted", &HistoricalReplayer::exhausted)
        .def("name", &HistoricalReplayer::name);

    // ── SimulationEngine ──────────────────────────────────────────────────────
    // Owns the book passed to its constructor (unique_ptr transfer — the
    // Python `book` object you pass in should not be reused afterwards, same
    // as passing a matcher into TreeOrderBook above).

    py::class_<SimulationEngine>(m, "SimulationEngine")
        .def(py::init<SimulationConfig, std::unique_ptr<OrderBookInterface>>(),
             py::arg("config"), py::arg("book"))
        .def("add_source", &SimulationEngine::add_source, py::arg("source"))
        .def("run", &SimulationEngine::run)
        .def("stop", &SimulationEngine::stop)
        .def("reset", &SimulationEngine::reset)
        .def("set_fill_callback", &SimulationEngine::set_fill_callback, py::arg("callback"))
        .def("set_cancel_callback", &SimulationEngine::set_cancel_callback, py::arg("callback"))
        .def("set_ack_callback", &SimulationEngine::set_ack_callback, py::arg("callback"))
        .def("set_snapshot_callback", &SimulationEngine::set_snapshot_callback, py::arg("callback"))
        .def("set_trigger_callback", &SimulationEngine::set_trigger_callback, py::arg("callback"))
        .def("set_tick_callback", &SimulationEngine::set_tick_callback, py::arg("callback"))
        .def("is_running", &SimulationEngine::is_running)
        .def("current_time", &SimulationEngine::current_time)
        .def("orders_processed", &SimulationEngine::orders_processed)
        .def("orders_rejected", &SimulationEngine::orders_rejected)
        .def("stops_triggered", &SimulationEngine::stops_triggered)
        .def("tick_count", &SimulationEngine::tick_count)
        .def("book", &SimulationEngine::book, py::return_value_policy::reference_internal)
        .def("config", &SimulationEngine::config, py::return_value_policy::reference_internal)
        .def("historical_replayer", &SimulationEngine::historical_replayer,
             py::return_value_policy::reference_internal);

    // ── Price utilities ──────────────────────────────────────────────────────

    m.attr("BASIS_POINTS_PER_UNIT") = BASIS_POINTS_PER_UNIT;
    m.def("to_basis_points", &to_basis_points, py::arg("price"),
          "Convert a double price to basis points (int64), rounded.");
    m.def("from_basis_points", &from_basis_points, py::arg("basis_points"),
          "Convert basis points back to a double price.");
    m.def("format_price", &format_price, py::arg("basis_points"),
          "Format a basis-point price as a human-readable string.");
    m.def("tick_floor", &tick_floor, py::arg("price"), py::arg("tick_size"));
    m.def("tick_ceil", &tick_ceil, py::arg("price"), py::arg("tick_size"));
}
