#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "allocator.h"

namespace test_helpers {

// Parameters for controlling the orderbook data generation process.
struct OrderbookParams {
    // Shape
    size_t time_steps = 150;      // Number of snapshots to generate (e.g., 15s at 10Hz)
    size_t depth_levels = 25;     // Number of levels PER SIDE (e.g., 25 bids and 25 asks)

    // Price Dynamics
    double base_price = 50000.0;
    double price_volatility = 0.0005;  // 0.05% relative change per step
    double hurst_exponent = 0.5;       // 0.5 for pure random walk, >0.5 for trending, <0.5 for mean-reverting
    double spread_pct = 0.0002;        // Bid-ask spread as a percentage of mid-price
    double tick_size = 0.01;

    // Price Distribution (for non-Gaussian returns)
    double skewness = -0.1;      // Negative skew is common in finance
    double kurtosis = 4.0;       // Fat tails (leptokurtic distribution)

    // Volume & Count Dynamics
    double base_volume = 50.0;
    double volume_depth_decay = 2.0;    // Exponential decay factor for volume as depth increases
    double volume_U_shape_factor = 0.4; // Strength of the U-shape (higher volume at edges)
    double count_to_volume_ratio = 0.1; // Avg number of orders per unit of volume

    // Misc
    uint32_t random_seed = 42;
};


// Holds the generated orderbook data and its shape information.
struct OrderbookTestData {
    cryptodd::memory::vector<float> data; // Flattened data: [time, level_and_side, feature]
    size_t time_steps;
    size_t depth_levels_per_side;
    size_t features; // Should be 3 (price, volume, count)
};


/**
 * @brief Generates realistic 3D orderbook data using a sophisticated hybrid model.
 *
 * This model combines Fractional Brownian Motion for price paths, a Cornish-Fisher
 * expansion for realistic returns (skew/kurtosis), and a complex, U-shaped
 * volume profile across the book depth.
 *
 * @param params The configuration structure for data generation.
 * @return An OrderbookTestData struct containing the flattened data and its dimensions.
 */
OrderbookTestData generate_hybrid_orderbook_data(const OrderbookParams& params);


/**
 * @brief Generates 3D orderbook data based on the Claude AI's proposed logic.
 *
 * Adapts the original logic to the standardized (P,V,C) feature set and
 * separates bid/ask sides.
 *
 * @param params The configuration structure for data generation.
 * @return An OrderbookTestData struct.
 */
OrderbookTestData generate_claude_style_orderbook_data(const OrderbookParams& params);


/**
 * @brief Generates 3D orderbook data based on the DeepSeek AI's proposed logic.
 *
 * Adapts the original logic, which focused on a 4-feature snapshot, to the
 * standardized (P,V,C) feature set and separates bid/ask sides.
 *
 * @param params The configuration structure for data generation.
 * @return An OrderbookTestData struct.
 */
OrderbookTestData generate_deepseek_style_orderbook_data(const OrderbookParams& params);


} // namespace test_helpers