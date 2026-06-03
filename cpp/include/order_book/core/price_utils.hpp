#pragma once

#include <cstdint>
#include <cmath>
#include <stdexcept>
#include <string>

namespace order_book {

// ─────────────────────────────────────────────────────────────────────────────
// Price representation
//
// All prices inside the engine are int64_t in basis points (hundredths of a
// cent, i.e. 1/10000 of one currency unit). Integer arithmetic eliminates
// floating-point rounding errors in matching logic.
//
// Range: int64_t supports prices up to ~$922,337,203,685,477.58 — more than
// sufficient for any real instrument.
//
// Basis point encoding:
//   1 currency unit  = 10,000 basis points
//   $1.00            = 10,000
//   $150.05          = 1,500,500
//   $0.0001          = 1
// ─────────────────────────────────────────────────────────────────────────────

inline constexpr int64_t BASIS_POINTS_PER_UNIT = 10'000;

// Convert a double price to basis points with rounding.
// Throws std::invalid_argument if price is negative or non-finite.
inline int64_t to_basis_points(double price) {
    if (!std::isfinite(price) || price < 0.0) {
        throw std::invalid_argument(
            "Price must be a finite non-negative value, got: " +
            std::to_string(price));
    }
    return static_cast<int64_t>(price * BASIS_POINTS_PER_UNIT + 0.5);
}

// Convert basis points back to double. Always exact for values within range.
inline double from_basis_points(int64_t bp) noexcept {
    return static_cast<double>(bp) / static_cast<double>(BASIS_POINTS_PER_UNIT);
}

// Format a basis-point price as a human-readable string, e.g. "150.0500"
inline std::string format_price(int64_t bp) {
    int64_t whole      = bp / BASIS_POINTS_PER_UNIT;
    int64_t fractional = bp % BASIS_POINTS_PER_UNIT;
    // Zero-pad fractional part to 4 digits
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%lld.%04lld",
                  static_cast<long long>(whole),
                  static_cast<long long>(fractional));
    return std::string(buf);
}

// Compute tick-aligned price (round down to nearest tick_size basis points).
// tick_size must be > 0.
inline int64_t tick_floor(int64_t price, int64_t tick_size) noexcept {
    return (price / tick_size) * tick_size;
}

inline int64_t tick_ceil(int64_t price, int64_t tick_size) noexcept {
    return ((price + tick_size - 1) / tick_size) * tick_size;
}

} // namespace order_book