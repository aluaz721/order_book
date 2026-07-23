#include "../../include/order_book/sources/historical_replayer.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace order_book {

namespace {

// ── Big-endian field readers ────────────────────────────────────────────────
// ITCH 5.0 is a big-endian binary protocol; all multi-byte integer fields
// (stock locate, tracking number, timestamp, order reference number, shares,
// price) are encoded MSB-first.

uint16_t read_be16(const unsigned char* p) noexcept {
    return (static_cast<uint16_t>(p[0]) << 8) | static_cast<uint16_t>(p[1]);
}

uint32_t read_be32(const unsigned char* p) noexcept {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8)  |
            static_cast<uint32_t>(p[3]);
}

// ITCH timestamps are a 6-byte (48-bit) nanoseconds-since-midnight field.
uint64_t read_be48(const unsigned char* p) noexcept {
    uint64_t v = 0;
    for (int i = 0; i < 6; ++i) {
        v = (v << 8) | static_cast<uint64_t>(p[i]);
    }
    return v;
}

uint64_t read_be64(const unsigned char* p) noexcept {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v = (v << 8) | static_cast<uint64_t>(p[i]);
    }
    return v;
}

// ITCH stock symbols are an 8-byte field, right-padded with ASCII spaces.
std::string read_stock_symbol(const unsigned char* p) {
    size_t len = 8;
    while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == '\0')) {
        --len;
    }
    return std::string(reinterpret_cast<const char*>(p), len);
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Construction / destruction
// ─────────────────────────────────────────────────────────────────────────────

HistoricalReplayer::HistoricalReplayer(Config config)
    : config_(std::move(config))
{
    auto* stream = new std::ifstream(config_.path, std::ios::binary);
    if (!stream->is_open()) {
        delete stream;
        throw std::runtime_error(
            "HistoricalReplayer: failed to open ITCH file: " + config_.path);
    }
    file_handle_ = stream;

    // Prime the lookahead buffer with the first surviving event.
    advance();
}

HistoricalReplayer::~HistoricalReplayer() {
    delete reinterpret_cast<std::ifstream*>(file_handle_);

    if (config_.verbose) {
        std::cerr << "[HistoricalReplayer] " << config_.path << " —"
                  << " messages_parsed=" << messages_parsed_
                  << " orders_replayed=" << orders_replayed_
                  << " orders_skipped="  << orders_skipped_
                  << " bytes_read="      << bytes_read_
                  << "\n";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// advance()
//
// Parses length-prefixed ITCH frames — [2-byte big-endian length][message
// bytes] — one at a time until either a message survives filtering (becomes
// the new lookahead_) or the file is exhausted. Messages that are filtered
// out (wrong symbol, outside the [start_time, end_time] window) or that
// aren't order events at all are consumed and skipped within this same call
// — the caller never sees them.
// ─────────────────────────────────────────────────────────────────────────────

void HistoricalReplayer::advance() {
    auto& stream = *reinterpret_cast<std::ifstream*>(file_handle_);

    unsigned char len_buf[2];
    unsigned char stack_buf[64];
    std::vector<unsigned char> heap_buf; // only used for an oversized message

    while (true) {
        stream.read(reinterpret_cast<char*>(len_buf), 2);
        if (!stream) {
            exhausted_ = true;
            lookahead_ = std::nullopt;
            return;
        }
        bytes_read_ += 2;

        const uint16_t msg_len = read_be16(len_buf);
        if (msg_len == 0) {
            exhausted_ = true;
            lookahead_ = std::nullopt;
            return;
        }

        unsigned char* buf = stack_buf;
        if (msg_len > sizeof(stack_buf)) {
            heap_buf.resize(msg_len);
            buf = heap_buf.data();
        }

        stream.read(reinterpret_cast<char*>(buf), msg_len);
        if (!stream) {
            exhausted_ = true;
            lookahead_ = std::nullopt;
            return;
        }
        bytes_read_ += msg_len;
        messages_parsed_++;

        // Every handled message type carries at least the common 11-byte
        // header: type(1) + stock locate(2) + tracking number(2) + timestamp(6).
        if (msg_len < 11) {
            continue; // truncated/malformed — skip
        }

        const unsigned char type      = buf[0];
        const uint64_t      timestamp = read_be48(buf + 5);

        if (timestamp < config_.start_time) {
            orders_skipped_++;
            continue;
        }
        if (config_.end_time > 0 && timestamp > config_.end_time) {
            exhausted_ = true;
            lookahead_ = std::nullopt;
            return;
        }

        std::optional<SourceEvent> event;

        switch (type) {

            // ── Add Order (No MPID) / Add Order with MPID Attribution ──────
            case 'A':
            case 'F': {
                if (msg_len < 36) continue; // malformed
                const uint64_t    order_ref = read_be64(buf + 11);
                const Side        side      = (buf[19] == 'B') ? Side::BID : Side::ASK;
                const uint64_t    shares    = read_be32(buf + 20);
                const std::string stock     = read_stock_symbol(buf + 24);
                const int64_t     price     = static_cast<int64_t>(read_be32(buf + 32));

                if (!config_.symbol_filter.empty() && stock != config_.symbol_filter) {
                    orders_skipped_++;
                    continue;
                }

                active_orders_[order_ref] = TrackedOrder{stock, side, shares};

                Order o{};
                o.id                 = order_ref;
                o.symbol             = stock;
                o.side               = side;
                o.type               = OrderType::LIMIT;
                o.price              = price;
                o.limit_price        = 0;
                o.quantity_remaining = shares;
                o.orig_quantity      = shares;
                o.timestamp          = timestamp;
                o.status             = OrderStatus::NEW;

                event = SourceEvent::new_order(std::move(o));
                break;
            }

            // ── Order Executed / Order Executed with Price ──────────────────
            case 'E':
            case 'C': {
                if (msg_len < 31) continue; // malformed
                const uint64_t order_ref       = read_be64(buf + 11);
                const uint64_t executed_shares = read_be32(buf + 19);

                auto it = active_orders_.find(order_ref);
                if (it == active_orders_.end()) {
                    // Unknown — either a filtered-out symbol or a reference
                    // to an order this replayer never saw. Either way, skip.
                    orders_skipped_++;
                    continue;
                }

                const uint64_t consumed =
                    std::min(executed_shares, it->second.remaining_qty);
                it->second.remaining_qty -= consumed;
                if (it->second.remaining_qty == 0) {
                    active_orders_.erase(it);
                }

                event = SourceEvent::execute(order_ref, executed_shares, timestamp);
                break;
            }

            // ── Order Cancel (partial — no trade, keeps time priority) ─────
            case 'X': {
                if (msg_len < 23) continue; // malformed
                const uint64_t order_ref        = read_be64(buf + 11);
                const uint64_t cancelled_shares = read_be32(buf + 19);

                auto it = active_orders_.find(order_ref);
                if (it == active_orders_.end()) {
                    orders_skipped_++;
                    continue;
                }

                const uint64_t consumed =
                    std::min(cancelled_shares, it->second.remaining_qty);
                it->second.remaining_qty -= consumed;
                if (it->second.remaining_qty == 0) {
                    active_orders_.erase(it);
                }

                event = SourceEvent::reduce(order_ref, cancelled_shares, timestamp);
                break;
            }

            // ── Order Delete (full removal) ─────────────────────────────────
            case 'D': {
                if (msg_len < 19) continue; // malformed
                const uint64_t order_ref = read_be64(buf + 11);

                auto it = active_orders_.find(order_ref);
                if (it == active_orders_.end()) {
                    orders_skipped_++;
                    continue;
                }
                active_orders_.erase(it);

                event = SourceEvent::cancel(order_ref, timestamp);
                break;
            }

            // ── Order Replace (cancel old ref, add new ref) ─────────────────
            case 'U': {
                if (msg_len < 35) continue; // malformed
                const uint64_t old_ref = read_be64(buf + 11);
                const uint64_t new_ref = read_be64(buf + 19);
                const uint64_t shares  = read_be32(buf + 27);
                const int64_t  price   = static_cast<int64_t>(read_be32(buf + 31));

                auto it = active_orders_.find(old_ref);
                if (it == active_orders_.end()) {
                    orders_skipped_++;
                    continue;
                }
                // Side and symbol aren't carried in the Replace message —
                // inherited from the order being replaced.
                const std::string stock = it->second.symbol;
                const Side        side  = it->second.side;
                active_orders_.erase(it);
                active_orders_[new_ref] = TrackedOrder{stock, side, shares};

                Order o{};
                o.id                 = new_ref;
                o.symbol             = stock;
                o.side               = side;
                o.type               = OrderType::LIMIT;
                o.price              = price;
                o.limit_price        = 0;
                o.quantity_remaining = shares;
                o.orig_quantity      = shares;
                o.timestamp          = timestamp;
                o.status             = OrderStatus::NEW;

                event = SourceEvent::replace(old_ref, std::move(o));
                break;
            }

            default:
                // 'S','H','Y','L','V','W','K','J','h','I','N','R','P','Q', or
                // any other type — not an order event. Skip silently.
                continue;
        }

        orders_replayed_++;
        lookahead_ = std::move(event);
        return;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// OrderSource interface
// ─────────────────────────────────────────────────────────────────────────────

uint64_t HistoricalReplayer::next_timestamp() const noexcept {
    if (lookahead_) return lookahead_->timestamp;
    return std::numeric_limits<uint64_t>::max();
}

std::optional<SourceEvent> HistoricalReplayer::next_order(uint64_t /*current_time*/) {
    if (!lookahead_) return std::nullopt;

    std::optional<SourceEvent> event = std::move(lookahead_);
    lookahead_ = std::nullopt;
    advance(); // refill the lookahead buffer for the next call
    return event;
}

bool HistoricalReplayer::exhausted() const noexcept {
    return exhausted_;
}

void HistoricalReplayer::on_book_update(const BookSnapshot&, uint64_t) {
    // HistoricalReplayer is a pure feed — it doesn't react to book state.
}

} // namespace order_book
