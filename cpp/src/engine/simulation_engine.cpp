#include "../../include/order_book/engine/simulation_engine.hpp"
#include "../../include/order_book/book/order_book_interface.hpp"
#include "../../include/order_book/orders/stop_order_manager.hpp"
#include "../../include/order_book/orders/order_validators.hpp"
#include "../../include/order_book/sources/order_source.hpp"
#include "../../include/order_book/sources/interactive_source.hpp"
#include "../../include/order_book/sources/historical_replayer.hpp"
#include "../../include/order_book/sources/strategy_source.hpp"

#include <limits>
#include <stdexcept>
#include <iostream>

namespace order_book {

// ─────────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────────

SimulationEngine::SimulationEngine(SimulationConfig                    config,
                                   std::unique_ptr<OrderBookInterface> book)
    : config_(std::move(config))
    , book_(std::move(book))
    , next_order_id_counter_(config_.order_id_start)
{
    if (!book_) {
        throw std::invalid_argument(
            "SimulationEngine: book must not be null");
    }

    // StopOrderManager holds a reference to book_ and shares the engine's
    // ID counter. Both lambdas capture `this` by pointer — safe because
    // stop_manager_ is owned by the engine and destroyed before book_.
    stop_manager_ = std::make_unique<StopOrderManager>(
        *book_,
        [this](const TriggerEvent& e) {
            stops_triggered_++;
            if (on_trigger_) on_trigger_(e);
        },
        [this](const CancelEvent& e) {
            if (on_cancel_) on_cancel_(e);
        },
        [this]() -> uint64_t {
            return next_order_id();
        }
    );

    // Wire book callbacks. Lambdas capture `this` so they always see the
    // current user-registered callbacks (on_fill_, on_cancel_, etc.) even
    // if the user calls set_*_callback() after construction.
    wire_callbacks();
}

// ─────────────────────────────────────────────────────────────────────────────
// wire_callbacks()
//
// Installs composite callbacks on the book. Each lambda:
//   1. Performs internal engine work (stop trigger check, source notification)
//   2. Forwards to the user-registered callback if one is set
//
// Called once at construction. Re-calling it is safe but unnecessary since
// the lambdas already capture `this` and pick up new user callbacks at call
// time via the on_fill_ / on_cancel_ / etc. members.
// ─────────────────────────────────────────────────────────────────────────────

void SimulationEngine::wire_callbacks() {
    OrderBookCallbacks cbs;

    // on_fill: check stop triggers before forwarding to user.
    // Calling stop_manager_->on_trade() here (rather than after book_->add()
    // returns) ensures triggered stops are processed while the fill event's
    // price and timestamp are in scope — and before the user's callback sees
    // the fill, preserving a consistent observable ordering.
    cbs.on_fill = [this](const FillEvent& e) {
        if (stop_manager_) {
            stop_manager_->on_trade(e.fill_price, e.timestamp);
        }
        if (on_fill_) on_fill_(e);
    };

    cbs.on_cancel = [this](const CancelEvent& e) {
        if (on_cancel_) on_cancel_(e);
    };

    cbs.on_ack = [this](const AckEvent& e) {
        if (on_ack_) on_ack_(e);
    };

    // on_book_update: cache the snapshot, notify all sources, forward to user.
    // notify_sources() is called here (not after dispatch() returns) because
    // the book emits this callback at the end of add() / cancel() — at that
    // point the book state is consistent and StrategySource should see it
    // before the engine moves on to the next source in the priority queue.
    cbs.on_book_update = [this](const BookSnapshot& snap) {
        last_snapshot_ = snap;
        notify_sources(snap, snap.timestamp);
        if (on_snapshot_) on_snapshot_(snap);
    };

    book_->set_callbacks(std::move(cbs));
}

// ─────────────────────────────────────────────────────────────────────────────
// Source registration
// ─────────────────────────────────────────────────────────────────────────────

void SimulationEngine::add_source(std::unique_ptr<OrderSource> source) {
    if (!source) return;
    sources_.push_back(std::move(source));
}

InteractiveSource* SimulationEngine::interactive_source() const noexcept {
    for (const auto& s : sources_) {
        if (auto* p = dynamic_cast<InteractiveSource*>(s.get())) return p;
    }
    return nullptr;
}

HistoricalReplayer* SimulationEngine::historical_replayer() const noexcept {
    for (const auto& s : sources_) {
        if (auto* p = dynamic_cast<HistoricalReplayer*>(s.get())) return p;
    }
    return nullptr;
}

StrategySource* SimulationEngine::strategy_source() const noexcept {
    for (const auto& s : sources_) {
        if (auto* p = dynamic_cast<StrategySource*>(s.get())) return p;
    }
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// rebuild_heap()
//
// Constructs the min-heap from scratch by querying every non-exhausted source.
// Called at the start of run() and after reset(). O(N log N) where N ≤ 3.
// ─────────────────────────────────────────────────────────────────────────────

void SimulationEngine::rebuild_heap() {
    // std::priority_queue has no clear() — swap with an empty instance.
    SourceHeap empty;
    std::swap(heap_, empty);

    for (size_t i = 0; i < sources_.size(); ++i) {
        if (sources_[i]->exhausted()) continue;
        const uint64_t ts = sources_[i]->next_timestamp();
        if (ts != std::numeric_limits<uint64_t>::max()) {
            heap_.push({ts, i});
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// notify_sources()
//
// Forwards the current BookSnapshot to every registered source.
// StrategySource reacts by invoking the Python callback and buffering the
// returned orders for the next tick. All other sources ignore this call.
//
// IMPORTANT: this is called from inside the on_book_update callback, which
// itself is called from inside book_->add() / book_->cancel(). We must not
// touch heap_ here — the run() loop owns the heap and will re-insert the
// source after dispatch() returns. Re-inserting here would create duplicate
// entries in the heap.
// ─────────────────────────────────────────────────────────────────────────────

void SimulationEngine::notify_sources(const BookSnapshot& snap,
                                      uint64_t            timestamp) {
    for (auto& source : sources_) {
        source->on_book_update(snap, timestamp);
    }
    // Deliberately do NOT re-insert into heap_ here. The run() loop
    // re-queries next_timestamp() on the processed source after dispatch()
    // returns and re-inserts it with the updated timestamp at that point.
    // Any other source that gained work (e.g. StrategySource buffered orders)
    // will be found during the next rebuild_heap() call or when the engine
    // loops around and re-checks all sources.
}

// ─────────────────────────────────────────────────────────────────────────────
// dispatch()
//
// Routes a single source event to the correct OrderBookInterface call (or the
// stop manager, for STOP/STOP_LIMIT new orders):
//
//   NEW_ORDER → STOP/STOP_LIMIT: StopOrderManager::submit()
//               everything else: OrderValidators::validate() → book_->add()
//   CANCEL    → book_->cancel(target_order_id)
//   REDUCE    → book_->reduce(target_order_id, quantity, timestamp)
//   EXECUTE   → book_->execute(target_order_id, quantity, timestamp)
//   REPLACE   → book_->replace(target_order_id, order)
//
// CANCEL/REDUCE/EXECUTE/REPLACE reference an order the book already knows
// about (from a historical feed or the UI), so they skip OrderValidators —
// validation only applies to brand-new orders entering the book.
// ─────────────────────────────────────────────────────────────────────────────

void SimulationEngine::dispatch(SourceEvent event) {
    switch (event.type) {
        case SourceEventType::NEW_ORDER: {
            Order order = std::move(event.order);

            // Route stop orders before field validation — the validator
            // doesn't handle stop/stop-limit and would reject them on the
            // price field check.
            if (order.type == OrderType::STOP ||
                order.type == OrderType::STOP_LIMIT) {
                stop_manager_->submit(std::move(order), on_ack_);
                return;
            }

            const auto result = OrderValidators::validate(order, *book_);
            if (!result.valid) {
                orders_rejected_++;
                if (on_ack_) {
                    on_ack_(AckEvent{
                        order.id,
                        order.symbol,
                        OrderStatus::REJECTED,
                        result.reason,
                        order.timestamp
                    });
                }
                return;
            }

            book_->add(std::move(order));
            return;
        }
        case SourceEventType::CANCEL:
            book_->cancel(event.target_order_id);
            return;
        case SourceEventType::REDUCE:
            book_->reduce(event.target_order_id, event.quantity, event.timestamp);
            return;
        case SourceEventType::EXECUTE:
            book_->execute(event.target_order_id, event.quantity, event.timestamp);
            return;
        case SourceEventType::REPLACE:
            book_->replace(event.target_order_id, std::move(event.order));
            return;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// run()
//
// Main simulation loop. Processes one event per outer iteration:
//
//   1. Pop the SourceEntry with the smallest next_timestamp from the heap.
//   2. Re-verify the source still has something (the cached timestamp in the
//      heap may be stale due to notify_sources() activity since last insert).
//   3. Call next_order() and dispatch() the result (add/cancel/reduce/
//      execute/replace — see SimulationEngine::dispatch()).
//   4. Re-insert the source into the heap with its updated next_timestamp.
//   5. Fire the tick callback.
//   6. Check termination conditions.
//
// Termination:
//   The loop exits when the heap is empty AND all non-exhausted sources have
//   nothing pending. For interactive-only use (MVP), this means run() returns
//   as soon as the InteractiveSource queue is drained. The API layer re-invokes
//   run() after each batch of submitted orders, or runs it in a background
//   thread and submits orders via InteractiveSource::submit().
//
// Historical replay:
//   HistoricalReplayer::exhausted() returns true after EOF. Once all
//   historical sources are exhausted and removed from the heap, only live
//   sources (InteractiveSource, StrategySource) remain. If they have nothing
//   pending, the loop exits. Callers running full simulations should check
//   orders_processed() to confirm all data was consumed.
// ─────────────────────────────────────────────────────────────────────────────

void SimulationEngine::run() {
    running_.store(true);
    rebuild_heap();

    while (running_.load()) {

        // ── Step 1: Find the source with the earliest pending event ───────────

        // Discard stale heap entries until we find a source that actually has
        // something ready. A stale entry occurs when a source was re-inserted
        // with timestamp T, then notify_sources() caused it to buffer more
        // orders — the heap still holds the old T but the source may now have
        // an earlier-than-T timestamp in its queue.
        bool dispatched = false;

        while (!heap_.empty() && running_.load()) {
            const SourceEntry top = heap_.top();
            heap_.pop();

            // Bounds guard — should never fire but prevents UB on bad state.
            if (top.index >= sources_.size()) continue;

            OrderSource* src = sources_[top.index].get();

            // Check end_time before doing any work.
            if (config_.end_time > 0 && top.next_ts > config_.end_time) {
                running_.store(false);
                break;
            }

            // Re-query to detect stale entries and handle false positives from
            // InteractiveSource's lock-free next_timestamp() check.
            const uint64_t live_ts = src->next_timestamp();
            if (live_ts == std::numeric_limits<uint64_t>::max()) {
                // Source has nothing right now. If it's not exhausted, it may
                // gain work later (e.g. more interactive orders submitted).
                // Don't re-insert — it will be picked up on the next
                // rebuild_heap() or added by a future notify_sources() call.
                continue;
            }

            // ── Step 2: Pop and dispatch the event ────────────────────────────

            auto event_opt = src->next_order(live_ts);
            if (!event_opt) {
                // Source returned nullopt — spurious wakeup (atomic false
                // positive). Re-insert if the source isn't exhausted.
                if (!src->exhausted()) {
                    const uint64_t new_ts = src->next_timestamp();
                    if (new_ts != std::numeric_limits<uint64_t>::max()) {
                        heap_.push({new_ts, top.index});
                    }
                }
                continue;
            }

            // Advance the simulation clock. Use the event's own timestamp if
            // set; fall back to the source's queried timestamp otherwise.
            current_time_ = (event_opt->timestamp > 0)
                            ? event_opt->timestamp
                            : live_ts;

            dispatch(std::move(*event_opt));
            orders_processed_++;

            // ── Step 3: Re-insert source with updated timestamp ───────────────

            if (!src->exhausted()) {
                const uint64_t new_ts = src->next_timestamp();
                if (new_ts != std::numeric_limits<uint64_t>::max()) {
                    heap_.push({new_ts, top.index});
                }
            }

            // ── Step 4: Re-insert any sources that gained work via notify ─────
            //
            // notify_sources() is called inside on_book_update (wired in
            // wire_callbacks()), which fires inside book_->add(). At that
            // point, StrategySource may have buffered new orders. Those sources
            // are not in the heap yet (we deliberately don't insert in
            // notify_sources to avoid double-insertion). We scan for them now.
            for (size_t i = 0; i < sources_.size(); ++i) {
                if (i == top.index) continue; // already re-inserted above
                if (sources_[i]->exhausted()) continue;
                const uint64_t candidate_ts = sources_[i]->next_timestamp();
                if (candidate_ts != std::numeric_limits<uint64_t>::max()) {
                    // Only insert if not already in the heap. Since we can't
                    // query the heap for membership, we always insert and rely
                    // on the stale-entry discard at the top of the inner loop.
                    heap_.push({candidate_ts, i});
                }
            }

            dispatched = true;
            break; // one order per outer while() iteration
        }

        // ── Step 6: Tick accounting ───────────────────────────────────────────

        tick_count_++;

        if (on_tick_) {
            on_tick_(current_time_, last_snapshot_);
        }

        // ── Step 7: Yield check ───────────────────────────────────────────────

        if (config_.yield_every_n_orders > 0 &&
            (orders_processed_ % config_.yield_every_n_orders) == 0) {
            // Cooperative yield — gives other threads a window to call stop()
            // or submit orders. Not a sleep; just a memory fence + reschedule
            // hint on platforms where the compiler generates one.
            running_.load(std::memory_order_seq_cst);
        }

        // ── Step 8: Termination check ─────────────────────────────────────────

        if (!dispatched) {
            // The heap is empty (or everything in it was stale/spurious).
            // Check whether any source might produce work in the future.
            bool any_non_exhausted = false;
            for (const auto& s : sources_) {
                if (!s->exhausted()) {
                    any_non_exhausted = true;
                    break;
                }
            }

            if (!any_non_exhausted) {
                // Every source has signalled exhaustion — we're done.
                break;
            }

            // At least one live source (InteractiveSource / StrategySource)
            // exists but is idle right now. Exit the loop and return to the
            // caller. For interactive MVP use:
            //   - The FastAPI endpoint calls engine.run() after each order
            //     submission, so the next call will drain the new order.
            //   - For background-thread use, the caller can loop on run() or
            //     block on a condition variable signalled by submit().
            break;
        }
    }

    running_.store(false);

    if (config_.verbose) {
        std::cerr << "[SimulationEngine] run() complete —"
                  << " orders_processed="  << orders_processed_
                  << " orders_rejected="   << orders_rejected_
                  << " stops_triggered="   << stops_triggered_
                  << " ticks="             << tick_count_
                  << "\n";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// stop()
// ─────────────────────────────────────────────────────────────────────────────

void SimulationEngine::stop() noexcept {
    running_.store(false, std::memory_order_release);
}

// ─────────────────────────────────────────────────────────────────────────────
// reset()
// ─────────────────────────────────────────────────────────────────────────────

void SimulationEngine::reset() {
    if (running_.load()) {
        throw std::logic_error(
            "SimulationEngine::reset() called while engine is running. "
            "Call stop() and wait for run() to return first.");
    }

    current_time_          = 0;
    orders_processed_      = 0;
    orders_rejected_       = 0;
    stops_triggered_       = 0;
    tick_count_            = 0;
    next_order_id_counter_ = config_.order_id_start;
    last_snapshot_         = BookSnapshot{};

    SourceHeap empty;
    std::swap(heap_, empty);
}

// ─────────────────────────────────────────────────────────────────────────────
// Callback setters
//
// All setters update a stored std::function. The lambdas in wire_callbacks()
// capture `this` and dereference on_fill_ etc. at call time, so new callbacks
// registered after construction are picked up automatically.
// ─────────────────────────────────────────────────────────────────────────────

void SimulationEngine::set_fill_callback(FillCallback cb) {
    on_fill_ = std::move(cb);
}

void SimulationEngine::set_cancel_callback(CancelCallback cb) {
    on_cancel_ = std::move(cb);
}

void SimulationEngine::set_ack_callback(AckCallback cb) {
    on_ack_ = std::move(cb);
}

void SimulationEngine::set_snapshot_callback(SnapshotCallback cb) {
    on_snapshot_ = std::move(cb);
}

void SimulationEngine::set_trigger_callback(TriggerCallback cb) {
    on_trigger_ = std::move(cb);
}

void SimulationEngine::set_tick_callback(TickCallback cb) {
    on_tick_ = std::move(cb);
}

} // namespace order_book