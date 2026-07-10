#include "../../include/order_book/book/vector_order_book.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// NOTE: Post-MVP stub.
// Built only when AQUILA_BUILD_VECTOR_BOOK=ON.
// Replace stub bodies with production implementations before enabling.
// All methods throw std::logic_error to catch accidental use during MVP phase.
// ─────────────────────────────────────────────────────────────────────────────

#include <stdexcept>

namespace order_book {

static constexpr auto NOT_IMPLEMENTED =
    "VectorOrderBook is post-MVP and not yet implemented. "
    "Use TreeOrderBook for the MVP build.";

VectorOrderBook::VectorOrderBook(Config                             config,
                                 std::unique_ptr<MatchingAlgorithm> matcher,
                                 OrderBookCallbacks                 callbacks)
    : config_(std::move(config))
    , matcher_(std::move(matcher))
    , callbacks_(std::move(callbacks))
{
    // Pre-allocate the flat price array.
    size_t total_levels = 2 * config_.half_capacity + 1;
    levels_.reserve(total_levels);
    for (size_t i = 0; i < total_levels; ++i) {
        int64_t price = config_.base_price +
                        (static_cast<int64_t>(i) - static_cast<int64_t>(config_.half_capacity))
                        * config_.tick_size;
        levels_.emplace_back(price);
    }
    best_bid_idx_ = SIZE_MAX;
    best_ask_idx_ = SIZE_MAX;
}

void VectorOrderBook::add(Order)                                     { throw std::logic_error(NOT_IMPLEMENTED); }
void VectorOrderBook::cancel(uint64_t)                               { throw std::logic_error(NOT_IMPLEMENTED); }
void VectorOrderBook::execute(uint64_t, uint64_t, uint64_t)          { throw std::logic_error(NOT_IMPLEMENTED); }
void VectorOrderBook::replace(uint64_t, Order)                       { throw std::logic_error(NOT_IMPLEMENTED); }

std::optional<int64_t> VectorOrderBook::best_bid()     const noexcept { return std::nullopt; }
std::optional<int64_t> VectorOrderBook::best_ask()     const noexcept { return std::nullopt; }
std::optional<int64_t> VectorOrderBook::spread()       const noexcept { return std::nullopt; }
std::optional<double>  VectorOrderBook::mid_price()    const noexcept { return std::nullopt; }
std::optional<double>  VectorOrderBook::weighted_mid() const noexcept { return std::nullopt; }

uint64_t VectorOrderBook::total_bid_qty()                  const noexcept { return 0; }
uint64_t VectorOrderBook::total_ask_qty()                  const noexcept { return 0; }
size_t   VectorOrderBook::bid_depth()                      const noexcept { return 0; }
size_t   VectorOrderBook::ask_depth()                      const noexcept { return 0; }
bool     VectorOrderBook::has_order(uint64_t)              const noexcept { return false; }
int64_t  VectorOrderBook::last_trade_price()               const noexcept { return 0; }
uint64_t VectorOrderBook::sequence()                       const noexcept { return sequence_; }

const std::string& VectorOrderBook::symbol()              const noexcept { return config_.symbol; }

BookSnapshot VectorOrderBook::snapshot(int) const {
    return BookSnapshot{config_.symbol, {}, {}, 0, 0, sequence_};
}

void VectorOrderBook::set_callbacks(OrderBookCallbacks cb)           { callbacks_ = std::move(cb); }
void VectorOrderBook::set_matching_algorithm(
    std::unique_ptr<MatchingAlgorithm> algo)                         { matcher_ = std::move(algo); }

void VectorOrderBook::recenter(int64_t)                              { throw std::logic_error(NOT_IMPLEMENTED); }

// Private stubs
size_t  VectorOrderBook::price_to_index(int64_t) const               { return 0; }
int64_t VectorOrderBook::index_to_price(size_t)  const               { return 0; }
bool    VectorOrderBook::is_bid_index(size_t)     const noexcept      { return false; }
void    VectorOrderBook::update_best_bid_down()                       {}
void    VectorOrderBook::update_best_ask_up()                         {}
void    VectorOrderBook::match(Order&)                                {}
bool    VectorOrderBook::can_fill_fully(const Order&) const noexcept  { return false; }
void    VectorOrderBook::emit_snapshot(uint64_t)                      {}

} // namespace order_book