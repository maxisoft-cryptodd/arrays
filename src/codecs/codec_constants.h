#pragma once

#include <cstddef>

namespace cryptodd::codecs {

/**
 * @brief Defines constants for various exchange-specific order book codecs.
 * This centralizes the "magic numbers" for depth and features, improving
 * readability and maintainability.
 */
namespace Orderbook {
    constexpr size_t OKX_DEPTH = 50;
    constexpr size_t OKX_FEATURES = 3;
    constexpr size_t BINANCE_DEPTH = 256;
    constexpr size_t BINANCE_FEATURES = 8;
} // namespace Orderbook

} // namespace cryptodd::codecs