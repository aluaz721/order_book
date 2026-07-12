// Build (Release is mandatory — Debug timings are meaningless):
//   cmake -B build -DCMAKE_BUILD_TYPE=Release
//   cmake --build build --target bench_order_book -j$(nproc)
//
// Run (produces JSON consumed by scripts/plot_benchmarks.py):
//   ./build/bench_order_book              \
//       --benchmark_repetitions=20        \
//       --benchmark_report_aggregates_only=false \
//       --benchmark_format=json           \
//       --benchmark_out=docs/bench_raw.json
//
//
// ── Mixed workload design ────────────────────────────────────────────────────
//
// BM_MixedWorkload is parameterised by TARGET_SIZE (resting orders):
//   1,000 / 10,000 / 100,000 / 1,000,000
//
// The book is pre-seeded to TARGET_SIZE before timing starts.  During the
// timed loop, every 10,000 operations the book is replenished back to
// TARGET_SIZE inside PauseTiming() so we benchmark a stationary workload,
// not a draining one.  Cancel targets are drawn from a deque in FIFO order
// (oldest orders cancelled first), which matches real market-making patterns.
//
// Workload mix:
//   40% resting limit add  (non-crossing — builds book depth)
//   35% cancel             (drawn from oldest resting orders)
//   20% market order       (removes liquidity — balanced by 40% adds)
//    5% IOC                (immediate match or cancel)
//
// ── Other benchmarks ─────────────────────────────────────────────────────────
//
//   BM_AddLimitOrder_NoMatch   Pure insert latency, no match. Baseline.
//   BM_AddLimitOrder_FullMatch Insert + one full match + FillEvent emission.
//   BM_AddMarketOrder_SweepN   Latency vs levels swept (cache miss curve).
//   BM_Cancel                  Hash map lookup + list erase path.
//   BM_AddIOC / BM_AddFOK      Immediate-or-cancel vs fill-or-kill.
//   BM_BestBidAsk              Query only — must be O(1).
//   BM_Snapshot                Serialise top-N levels (WebSocket push cost).
//   BM_HighContentionMatch     Single price level, maximum match rate.

#include <benchmark/benchmark.h>

#include "../include/order_book/book/tree_order_book.hpp"
#include "../include/order_book/matching/fifo_matcher.hpp"
#include "../include/order_book/core/price_utils.hpp"

#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace {

using namespace order_book;
using Clock = std::chrono::steady_clock;

// ─────────────────────────────────────────────────────────────────────────────
// Shared infrastructure
// ─────────────────────────────────────────────────────────────────────────────

static constexpr std::string_view SYMBOL  = "AAPL";
static constexpr int64_t          MID_BP  = 1'500'000; // $150.00

static OrderBookCallbacks silent_callbacks() { return {}; }

static std::unique_ptr<TreeOrderBook> make_book() {
    return std::make_unique<TreeOrderBook>(
        std::string(SYMBOL),
        std::make_unique<FIFOMatcher>(),
        silent_callbacks()
    );
}

static Order make_order(uint64_t  id,
                        Side      side,
                        OrderType type,
                        int64_t   price_bp,
                        uint64_t  qty)
{
    Order o{};
    o.id                      = id;
    o.symbol                  = std::string(SYMBOL);
    o.side                    = side;
    o.type                    = type;
    o.price                   = price_bp;
    o.limit_price             = 0;
    o.quantity_remaining      = qty;
    o.orig_quantity           = qty;
    o.timestamp               = 1'000'000'000ULL;
    o.status                  = OrderStatus::NEW;
    return o;
}

// Returns a random non-crossing bid price below MID_BP.
// Prices are distributed uniformly across a ±5% band below mid so the book
// has realistic depth rather than all orders piling at one level.
static int64_t rand_bid_price(std::mt19937_64& rng) {
    // Bid range: [MID - $7.50, MID - $0.10] in basis points
    constexpr int64_t LO = MID_BP - 75'000;
    constexpr int64_t HI = MID_BP -  1'000;
    return LO + static_cast<int64_t>(rng() % (HI - LO));
}

static int64_t rand_ask_price(std::mt19937_64& rng) {
    constexpr int64_t LO = MID_BP +  1'000;
    constexpr int64_t HI = MID_BP + 75'000;
    return LO + static_cast<int64_t>(rng() % (HI - LO));
}

static uint64_t rand_qty(std::mt19937_64& rng) {
    return 1 + rng() % 500;
}

// Seed the book with `n` resting orders (roughly half bid, half ask).
// Returns the next free order ID and populates resting_ids with all added IDs.
static uint64_t seed_book_n(TreeOrderBook&         book,
                             std::deque<uint64_t>&  resting_ids,
                             uint64_t               start_id,
                             uint64_t               n,
                             std::mt19937_64&       rng)
{
    uint64_t id = start_id;
    for (uint64_t i = 0; i < n; ++i) {
        Side side = (i % 2 == 0) ? Side::BID : Side::ASK;
        int64_t price = (side == Side::BID) ? rand_bid_price(rng)
                                            : rand_ask_price(rng);
        book.add(make_order(id, side, OrderType::LIMIT, price, rand_qty(rng)));
        resting_ids.push_back(id);
        ++id;
    }
    return id;
}

// Replenish the book back to target_size during PauseTiming().
// New IDs are appended to resting_ids for future cancel targeting.
static uint64_t replenish(TreeOrderBook&        book,
                           std::deque<uint64_t>& resting_ids,
                           uint64_t              next_id,
                           uint64_t              target_size,
                           std::mt19937_64&      rng)
{
    while (resting_ids.size() < target_size) {
        Side side = (next_id % 2 == 0) ? Side::BID : Side::ASK;
        int64_t price = (side == Side::BID) ? rand_bid_price(rng)
                                            : rand_ask_price(rng);
        book.add(make_order(next_id, side, OrderType::LIMIT,
                            price, rand_qty(rng)));
        resting_ids.push_back(next_id);
        ++next_id;
    }
    return next_id;
}

// ─────────────────────────────────────────────────────────────────────────────
// BM_MixedWorkload
//
// Parameterised by target book size (resting orders): 1k, 10k, 100k, 1M.
// Workload: 40% limit add, 35% cancel, 20% market, 5% IOC.
// Replenishes every 10,000 ops inside PauseTiming().
// ─────────────────────────────────────────────────────────────────────────────

static void BM_MixedWorkload(benchmark::State& state) {
    const uint64_t TARGET_SIZE = static_cast<uint64_t>(state.range(0));
    constexpr uint64_t REPLENISH_INTERVAL = 10'000;

    std::mt19937_64 rng(42);
    auto book = make_book();
    std::deque<uint64_t> resting_ids;

    // ── Pre-seed — excluded from timing ──────────────────────────────────────
    uint64_t next_id = seed_book_n(*book, resting_ids, 1, TARGET_SIZE, rng);

    // ── Per-iteration latency samples (accumulated across all iterations) ────
    std::vector<double> latencies_ns;
    latencies_ns.reserve(1'000'000);

    uint64_t op_count     = 0;
    uint64_t ops_limit    = 0;
    uint64_t ops_market   = 0;
    uint64_t ops_cancel   = 0;
    uint64_t ops_ioc      = 0;

    // Wall-clock start for true throughput denominator
    auto wall_start = Clock::now();
    bool first_iter = true;

    for (auto _ : state) {

        // ── Replenish every 10k ops (paused, not timed) ───────────────────
        if (op_count % REPLENISH_INTERVAL == 0 && op_count > 0) {
            state.PauseTiming();
            next_id = replenish(*book, resting_ids, next_id, TARGET_SIZE, rng);
            state.ResumeTiming();
        }

        if (first_iter) {
            wall_start = Clock::now();
            first_iter = false;
        }

        // ── Select operation ──────────────────────────────────────────────
        uint64_t roll = rng() % 100;

        auto t0 = Clock::now();

        if (roll < 40) {
            // 40% — resting limit add (non-crossing)
            Side side = (rng() % 2 == 0) ? Side::BID : Side::ASK;
            int64_t p = (side == Side::BID) ? rand_bid_price(rng)
                                            : rand_ask_price(rng);
            book->add(make_order(next_id, side, OrderType::LIMIT,
                                 p, rand_qty(rng)));
            resting_ids.push_back(next_id);
            ++next_id;
            ++ops_limit;

        } else if (roll < 75) {
            // 35% — cancel oldest resting order
            if (!resting_ids.empty()) {
                uint64_t target = resting_ids.front();
                resting_ids.pop_front();
                book->cancel(target);
            }
            ++ops_cancel;

        } else if (roll < 95) {
            // 20% — market order (small qty, unlikely to drain book)
            Side side = (rng() % 2 == 0) ? Side::BID : Side::ASK;
            uint64_t qty = 1 + rng() % 10;   // small to avoid over-draining
            book->add(make_order(next_id++, side, OrderType::MARKET, 0, qty));
            ++ops_market;

        } else {
            // 5% — IOC (crosses spread if possible, cancels remainder)
            int64_t p = MID_BP + static_cast<int64_t>(rng() % 10'000) - 5'000;
            book->add(make_order(next_id++, Side::BID,
                                 OrderType::IMMEDIATE_OR_CANCEL, p,
                                 rand_qty(rng)));
            ++ops_ioc;
        }

        auto t1 = Clock::now();
        latencies_ns.push_back(
            static_cast<double>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()
            )
        );
        ++op_count;
    }

    auto wall_end  = Clock::now();
    double wall_ns = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            wall_end - wall_start).count()
    );

    // ── Report throughput = total_ops / total_wall_time ───────────────────
    // SetItemsProcessed is used by Google Benchmark to compute items_per_second
    // in the JSON: items_per_second = iterations / elapsed_time.
    // We additionally embed our own wall-clock rate in the label string so
    // the plotting script can display both.
    state.SetItemsProcessed(static_cast<int64_t>(op_count));

    // ── Compute latency percentiles from raw samples ───────────────────────
    std::sort(latencies_ns.begin(), latencies_ns.end());
    const size_t n = latencies_ns.size();

    auto percentile = [&](double p) -> double {
        if (n == 0) return 0.0;
        size_t idx = std::min(static_cast<size_t>(p / 100.0 * n), n - 1);
        return latencies_ns[idx];
    };

    double median = percentile(50);
    double p95    = percentile(95);
    double p99    = percentile(99);
    double p999   = percentile(99.9);
    double maxlat = latencies_ns.empty() ? 0.0 : latencies_ns.back();
    double tput   = wall_ns > 0 ? (op_count / wall_ns) * 1e9 : 0.0;

    // Embed all stats in the label — the plotting script parses this.
    // Format: "key=value|key=value|..."
    std::string label =
        "target_size="  + std::to_string(TARGET_SIZE)    + "|"
        "ops="          + std::to_string(op_count)        + "|"
        "tput="         + std::to_string(static_cast<uint64_t>(tput)) + "|"
        "median_ns="    + std::to_string(static_cast<uint64_t>(median)) + "|"
        "p95_ns="       + std::to_string(static_cast<uint64_t>(p95))   + "|"
        "p99_ns="       + std::to_string(static_cast<uint64_t>(p99))   + "|"
        "p999_ns="      + std::to_string(static_cast<uint64_t>(p999))  + "|"
        "max_ns="       + std::to_string(static_cast<uint64_t>(maxlat))+ "|"
        "pct_limit="    + std::to_string(ops_limit  * 100 / std::max(op_count, uint64_t(1))) + "|"
        "pct_cancel="   + std::to_string(ops_cancel * 100 / std::max(op_count, uint64_t(1))) + "|"
        "pct_market="   + std::to_string(ops_market * 100 / std::max(op_count, uint64_t(1))) + "|"
        "pct_ioc="      + std::to_string(ops_ioc    * 100 / std::max(op_count, uint64_t(1)));

    state.SetLabel(label);
}

// Four book sizes. Run 5 repetitions each — mixed workload is slow at 1M.
BENCHMARK(BM_MixedWorkload)
    ->Arg(1'000)
    ->Arg(10'000)
    ->Arg(100'000)
    ->Arg(1'000'000)
    ->Repetitions(5)
    ->ReportAggregatesOnly(false)
    ->Unit(benchmark::kNanosecond);

// ─────────────────────────────────────────────────────────────────────────────
// BM_AddLimitOrder_NoMatch
// Pure insert latency with no matching — baseline for everything else.
// Book is pre-seeded to ~10k orders so cache behaviour is realistic.
// ─────────────────────────────────────────────────────────────────────────────

static void BM_AddLimitOrder_NoMatch(benchmark::State& state) {
    std::mt19937_64 rng(1);
    auto book    = make_book();
    std::deque<uint64_t> dummy;
    uint64_t next_id = seed_book_n(*book, dummy, 1, 10'000, rng);

    for (auto _ : state) {
        // Bids placed well below best ask — guaranteed no cross.
        int64_t p = rand_bid_price(rng);
        book->add(make_order(next_id++, Side::BID, OrderType::LIMIT, p, 10));
    }
    state.SetItemsProcessed(state.iterations());
    state.SetLabel("add/no-match");
}
BENCHMARK(BM_AddLimitOrder_NoMatch)
    ->Repetitions(20)->ReportAggregatesOnly(false)->Unit(benchmark::kNanosecond);

// ─────────────────────────────────────────────────────────────────────────────
// BM_AddLimitOrder_FullMatch
// One aggressive limit that exactly matches one resting limit.
// Book is reconstructed each iteration (PauseTiming) to avoid drain.
// ─────────────────────────────────────────────────────────────────────────────

static void BM_AddLimitOrder_FullMatch(benchmark::State& state) {
    uint64_t passive_id = 1;
    uint64_t aggress_id = 10'000'000;

    for (auto _ : state) {
        state.PauseTiming();
        auto book = make_book();
        book->add(make_order(passive_id++, Side::ASK, OrderType::LIMIT,
                             MID_BP, 100));
        state.ResumeTiming();

        book->add(make_order(aggress_id++, Side::BID, OrderType::LIMIT,
                             MID_BP, 100));
    }
    state.SetItemsProcessed(state.iterations());
    state.SetLabel("add/full-match");
}
BENCHMARK(BM_AddLimitOrder_FullMatch)
    ->Repetitions(20)->ReportAggregatesOnly(false)->Unit(benchmark::kNanosecond);

// ─────────────────────────────────────────────────────────────────────────────
// BM_AddMarketOrder_SweepN
// Market order sweeping N distinct price levels.
// Slope of latency vs N reveals cache miss cost of traversing the price map.
// ─────────────────────────────────────────────────────────────────────────────

static void BM_AddMarketOrder_SweepN(benchmark::State& state) {
    const int N = static_cast<int>(state.range(0));
    uint64_t passive_id = 1;
    uint64_t aggress_id = 20'000'000;

    for (auto _ : state) {
        state.PauseTiming();
        auto book = make_book();
        for (int l = 0; l < N; ++l) {
            int64_t ask_price = MID_BP + (l + 1) * 1'000;
            book->add(make_order(passive_id++, Side::ASK, OrderType::LIMIT,
                                 ask_price, 100));
        }
        state.ResumeTiming();

        book->add(make_order(aggress_id++, Side::BID, OrderType::MARKET,
                             0, static_cast<uint64_t>(N) * 100));
    }
    state.SetItemsProcessed(state.iterations());
    state.SetLabel("market/sweep-" + std::to_string(N));
}
BENCHMARK(BM_AddMarketOrder_SweepN)
    ->Arg(1)->Arg(4)->Arg(8)->Arg(16)
    ->Repetitions(20)->ReportAggregatesOnly(false)->Unit(benchmark::kNanosecond);

// ─────────────────────────────────────────────────────────────────────────────
// BM_Cancel
// Cancel a resting order by ID against a book of 10k orders.
// Tests the hot path: unordered_map lookup → list erase → map prune if empty.
// ─────────────────────────────────────────────────────────────────────────────

static void BM_Cancel(benchmark::State& state) {
    constexpr uint64_t POOL = 10'000;
    std::mt19937_64 rng(2);
    auto book = make_book();
    std::deque<uint64_t> ids;
    uint64_t next_id = seed_book_n(*book, ids, 1, POOL, rng);

    uint64_t idx = 0;
    for (auto _ : state) {
        if (idx >= ids.size()) {
            state.PauseTiming();
            next_id = replenish(*book, ids, next_id, POOL, rng);
            idx = 0;
            state.ResumeTiming();
        }
        book->cancel(ids[idx++]);
    }
    state.SetItemsProcessed(state.iterations());
    state.SetLabel("cancel");
}
BENCHMARK(BM_Cancel)
    ->Repetitions(20)->ReportAggregatesOnly(false)->Unit(benchmark::kNanosecond);

// ─────────────────────────────────────────────────────────────────────────────
// BM_AddIOC / BM_AddFOK
// ─────────────────────────────────────────────────────────────────────────────

static void BM_AddIOC(benchmark::State& state) {
    uint64_t passive_id = 1;
    uint64_t aggress_id = 30'000'000;

    for (auto _ : state) {
        state.PauseTiming();
        auto book = make_book();
        book->add(make_order(passive_id++, Side::ASK, OrderType::LIMIT,
                             MID_BP, 50));
        state.ResumeTiming();

        book->add(make_order(aggress_id++, Side::BID,
                             OrderType::IMMEDIATE_OR_CANCEL, MID_BP, 100));
    }
    state.SetItemsProcessed(state.iterations());
    state.SetLabel("ioc");
}
BENCHMARK(BM_AddIOC)
    ->Repetitions(20)->ReportAggregatesOnly(false)->Unit(benchmark::kNanosecond);

static void BM_AddFOK_Fillable(benchmark::State& state) {
    uint64_t passive_id = 1;
    uint64_t aggress_id = 40'000'000;

    for (auto _ : state) {
        state.PauseTiming();
        auto book = make_book();
        book->add(make_order(passive_id++, Side::ASK, OrderType::LIMIT,
                             MID_BP, 100));
        state.ResumeTiming();

        book->add(make_order(aggress_id++, Side::BID,
                             OrderType::FILL_OR_KILL, MID_BP, 100));
    }
    state.SetItemsProcessed(state.iterations());
    state.SetLabel("fok/fillable");
}
BENCHMARK(BM_AddFOK_Fillable)
    ->Repetitions(20)->ReportAggregatesOnly(false)->Unit(benchmark::kNanosecond);

static void BM_AddFOK_Cancelled(benchmark::State& state) {
    uint64_t passive_id = 1;
    uint64_t aggress_id = 50'000'000;

    for (auto _ : state) {
        state.PauseTiming();
        auto book = make_book();
        book->add(make_order(passive_id++, Side::ASK, OrderType::LIMIT,
                             MID_BP, 50));
        state.ResumeTiming();

        book->add(make_order(aggress_id++, Side::BID,
                             OrderType::FILL_OR_KILL, MID_BP, 100));
    }
    state.SetItemsProcessed(state.iterations());
    state.SetLabel("fok/cancelled");
}
BENCHMARK(BM_AddFOK_Cancelled)
    ->Repetitions(20)->ReportAggregatesOnly(false)->Unit(benchmark::kNanosecond);

// ─────────────────────────────────────────────────────────────────────────────
// BM_BestBidAsk — O(1) query; any regression is a design bug
// ─────────────────────────────────────────────────────────────────────────────

static void BM_BestBidAsk(benchmark::State& state) {
    std::mt19937_64 rng(3);
    auto book = make_book();
    std::deque<uint64_t> dummy;
    seed_book_n(*book, dummy, 1, 10'000, rng);

    for (auto _ : state) {
        benchmark::DoNotOptimize(book->best_bid());
        benchmark::DoNotOptimize(book->best_ask());
    }
    state.SetItemsProcessed(state.iterations());
    state.SetLabel("best_bid_ask");
}
BENCHMARK(BM_BestBidAsk)
    ->Repetitions(20)->ReportAggregatesOnly(false)->Unit(benchmark::kNanosecond);

// ─────────────────────────────────────────────────────────────────────────────
// BM_Snapshot — cost of one WebSocket push (snapshot() call)
// ─────────────────────────────────────────────────────────────────────────────

static void BM_Snapshot(benchmark::State& state) {
    const int depth = static_cast<int>(state.range(0));
    std::mt19937_64 rng(4);
    auto book = make_book();
    std::deque<uint64_t> dummy;
    seed_book_n(*book, dummy, 1, 10'000, rng);

    for (auto _ : state) {
        benchmark::DoNotOptimize(book->snapshot(depth));
    }
    state.SetItemsProcessed(state.iterations());
    state.SetLabel("snapshot/depth-" + std::to_string(depth));
}
BENCHMARK(BM_Snapshot)
    ->Arg(5)->Arg(10)->Arg(20)
    ->Repetitions(20)->ReportAggregatesOnly(false)->Unit(benchmark::kNanosecond);

// ─────────────────────────────────────────────────────────────────────────────
// BM_HighContentionMatch
// All orders at one price — stresses the linked-list head and FillEvent path.
// ─────────────────────────────────────────────────────────────────────────────

static void BM_HighContentionMatch(benchmark::State& state) {
    constexpr int64_t PRICE = MID_BP;
    auto book = make_book();
    uint64_t next_id = 1;

    // Pre-load passive asks at the single price
    for (int i = 0; i < 10'000; ++i)
        book->add(make_order(next_id++, Side::ASK, OrderType::LIMIT, PRICE, 1));

    for (auto _ : state) {
        if (book->ask_depth() == 0) {
            state.PauseTiming();
            for (int i = 0; i < 10'000; ++i)
                book->add(make_order(next_id++, Side::ASK, OrderType::LIMIT, PRICE, 1));
            state.ResumeTiming();
        }
        book->add(make_order(next_id++, Side::BID, OrderType::LIMIT, PRICE, 1));
    }
    state.SetItemsProcessed(state.iterations());
    state.SetLabel("high-contention");
}
BENCHMARK(BM_HighContentionMatch)
    ->Repetitions(20)->ReportAggregatesOnly(false)->Unit(benchmark::kNanosecond);

} // namespace

BENCHMARK_MAIN();