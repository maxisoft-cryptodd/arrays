#include "orderbook_generator.h"
#include <random>
#include <cmath>
#include <numeric>
#include <algorithm>
#include <stdexcept>

namespace test_helpers {

namespace { // Anonymous namespace for internal helpers

// Generates a Fractional Brownian Motion path.
std::vector<double> generate_fbm(size_t n, double hurst, std::mt19937& gen) {
    if (n == 0) return {};
    std::vector<double> fbm(n);
    std::normal_distribution<double> normal(0.0, 1.0);
    
    std::vector<double> cov(n);
    for (size_t i = 0; i < n; ++i) {
        cov[i] = 0.5 * (std::pow(i + 1, 2 * hurst) + std::pow(std::abs(static_cast<double>(i) - 1), 2 * hurst) - 2 * std::pow(i, 2 * hurst));
    }

    std::vector<std::vector<double>> cholesky(n, std::vector<double>(n, 0.0));
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = 0; j <= i; ++j) {
            double sum = 0.0;
            for (size_t k = 0; k < j; ++k) {
                sum += cholesky[i][k] * cholesky[j][k];
            }
            if (i == j) {
                cholesky[i][j] = std::sqrt(std::max(0.0, std::pow(i + 1, 2 * hurst) - sum));
            } else {
                 double cross_cov = 0.5 * (std::pow(i + 1, 2 * hurst) + std::pow(static_cast<double>(j) + 1, 2 * hurst) - std::pow(static_cast<double>(i-j), 2 * hurst) );
                if (cholesky[j][j] > 1e-10) {
                    cholesky[i][j] = (cross_cov - sum) / cholesky[j][j];
                }
            }
        }
    }

    std::vector<double> noise(n);
    for(size_t i=0; i<n; ++i) noise[i] = normal(gen);

    for (size_t i = 0; i < n; ++i) {
        fbm[i] = 0.0;
        for (size_t j = 0; j <= i; ++j) {
            fbm[i] += cholesky[i][j] * noise[j];
        }
    }
    return fbm;
}


// Applies skewness and kurtosis to a standard normal value using the Cornish-Fisher expansion.
double apply_cornish_fisher(double z, double skew, double kurtosis) {
    double k_excess = kurtosis - 3.0; // Excess kurtosis
    return z +
           (skew / 6.0) * (z * z - 1.0) +
           (k_excess / 24.0) * (z * z * z - 3.0 * z) -
           (skew * skew / 36.0) * (2.0 * z * z * z - 5.0 * z);
}

} // namespace


OrderbookTestData generate_hybrid_orderbook_data(const OrderbookParams& params) {
    const size_t time_steps = params.time_steps;
    const size_t depth = params.depth_levels;
    const size_t features = 3; // Price, Volume, Count

    OrderbookTestData result;
    result.time_steps = time_steps;
    result.depth_levels_per_side = depth;
    result.features = features;
    result.data.resize(time_steps * depth * 2 * features);

    std::mt19937 gen(params.random_seed);
    std::normal_distribution<double> normal(0.0, 1.0);
    std::uniform_real_distribution<double> uniform(0.9, 1.1);

    // --- 1. Generate Mid-Price Evolution ---
    auto fbm_path = generate_fbm(time_steps, params.hurst_exponent, gen);
    std::vector<double> mid_prices(time_steps);
    
    if (!fbm_path.empty()) {
        double fbm_mean = std::accumulate(fbm_path.begin(), fbm_path.end(), 0.0) / time_steps;
        double fbm_sq_sum = std::inner_product(fbm_path.begin(), fbm_path.end(), fbm_path.begin(), 0.0);
        double fbm_stdev = std::sqrt(fbm_sq_sum / time_steps - fbm_mean * fbm_mean);

        for (size_t t = 0; t < time_steps; ++t) {
            double z = (fbm_stdev > 1e-9) ? (fbm_path[t] - fbm_mean) / fbm_stdev : 0.0;
            double adjusted_z = apply_cornish_fisher(z, params.skewness, params.kurtosis);
            double price_return = adjusted_z * params.price_volatility;
            mid_prices[t] = params.base_price * (1.0 + price_return);
        }
    }

    // --- 2. Generate Snapshots for each Time Step ---
    for (size_t t = 0; t < time_steps; ++t) {
        double mid_price = mid_prices[t];
        double spread = mid_price * params.spread_pct * uniform(gen);
        double best_bid = std::floor((mid_price - spread / 2.0) / params.tick_size) * params.tick_size;
        double best_ask = std::ceil((mid_price + spread / 2.0) / params.tick_size) * params.tick_size;

        for (size_t l = 0; l < depth; ++l) {
            size_t bid_base_idx = (t * depth * 2 + l) * features;
            size_t ask_base_idx = (t * depth * 2 + depth + l) * features;
            double norm_level = static_cast<double>(l) / (depth - 1);

            // --- Volume and Count ---
            double decay = std::exp(-params.volume_depth_decay * norm_level);
            double u_shape = 1.0 + params.volume_U_shape_factor * std::pow(norm_level - 0.5, 2.0);
            double volume_factor = decay * u_shape;
            
            double bid_volume = params.base_volume * volume_factor * uniform(gen);
            double ask_volume = params.base_volume * volume_factor * uniform(gen);

            std::poisson_distribution<int> count_dist(std::max(1.0, bid_volume * params.count_to_volume_ratio));
            double bid_count = count_dist(gen);
            count_dist = std::poisson_distribution<int>(std::max(1.0, ask_volume * params.count_to_volume_ratio));
            double ask_count = count_dist(gen);
            
            // --- Prices ---
            double bid_price = best_bid - l * params.tick_size;
            double ask_price = best_ask + l * params.tick_size;

            // --- Assign to Flat Vector ---
            // Bid Side
            result.data[bid_base_idx + 0] = static_cast<float>(bid_price);
            result.data[bid_base_idx + 1] = static_cast<float>(bid_volume);
            result.data[bid_base_idx + 2] = static_cast<float>(bid_count);
            // Ask Side
            result.data[ask_base_idx + 0] = static_cast<float>(ask_price);
            result.data[ask_base_idx + 1] = static_cast<float>(ask_volume);
            result.data[ask_base_idx + 2] = static_cast<float>(ask_count);
        }
    }

    return result;
}


OrderbookTestData generate_claude_style_orderbook_data(const OrderbookParams& params) {
    const size_t time_steps = params.time_steps;
    const size_t depth = params.depth_levels;
    const size_t features = 3;

    OrderbookTestData result;
    result.time_steps = time_steps;
    result.depth_levels_per_side = depth;
    result.features = features;
    result.data.resize(time_steps * depth * 2 * features);
    
    std::mt19937 gen(params.random_seed);
    std::normal_distribution<double> normal(0.0, 1.0);
    
    auto price_fbm = generate_fbm(time_steps, params.hurst_exponent, gen);
    double price_mean = std::accumulate(price_fbm.begin(), price_fbm.end(), 0.0) / time_steps;
    double price_std = std::sqrt(std::inner_product(price_fbm.begin(), price_fbm.end(), price_fbm.begin(), 0.0) / time_steps - price_mean * price_mean);

    std::vector<double> normalized_prices(time_steps);
    for (size_t t = 0; t < time_steps; ++t) {
        double z = (price_std > 1e-10) ? (price_fbm[t] - price_mean) / price_std : 0.0;
        double adjusted_z = apply_cornish_fisher(z, params.skewness, params.kurtosis);
        normalized_prices[t] = params.base_price * (1.0 + adjusted_z * params.price_volatility);
    }
    
    for (size_t t = 0; t < time_steps; ++t) {
        double mid_price = normalized_prices[t];
        std::uniform_real_distribution<double> spread_dist(0.0001, 0.0003);
        double half_spread = mid_price * spread_dist(gen);
        
        for (size_t l = 0; l < depth; ++l) {
            size_t bid_base_idx = (t * depth * 2 + l) * features;
            size_t ask_base_idx = (t * depth * 2 + depth + l) * features;
            
            // --- Prices ---
            double tick_size = params.tick_size;
            double bid_offset = (l + 1) * tick_size;
            double ask_offset = (l + 1) * tick_size;
            double bid_price = mid_price - half_spread - bid_offset;
            double ask_price = mid_price + half_spread + ask_offset;
            
            // --- Volume and Count (U-shape) ---
            double normalized_level = static_cast<double>(l) / (depth - 1);
            double distance_from_mid = std::abs(normalized_level - 0.5) * 2.0;
            double volume_multiplier = 1.0 + (1.5 - 1.0) * distance_from_mid; // Edge multiplier of 1.5
            
            std::uniform_real_distribution<double> time_noise(0.9, 1.1);
            double bid_volume = params.base_volume * volume_multiplier * time_noise(gen);
            double ask_volume = params.base_volume * volume_multiplier * time_noise(gen);
            
            double bid_count = std::max(1.0, bid_volume * params.count_to_volume_ratio);
            double ask_count = std::max(1.0, ask_volume * params.count_to_volume_ratio);

            // --- Assign to Flat Vector ---
            result.data[bid_base_idx + 0] = static_cast<float>(bid_price);
            result.data[bid_base_idx + 1] = static_cast<float>(bid_volume);
            result.data[bid_base_idx + 2] = static_cast<float>(bid_count);
            result.data[ask_base_idx + 0] = static_cast<float>(ask_price);
            result.data[ask_base_idx + 1] = static_cast<float>(ask_volume);
            result.data[ask_base_idx + 2] = static_cast<float>(ask_count);
        }
    }
    
    return result;
}

OrderbookTestData generate_deepseek_style_orderbook_data(const OrderbookParams& params) {
    const size_t time_steps = params.time_steps;
    const size_t depth = params.depth_levels;
    const size_t features = 3;

    OrderbookTestData result;
    result.time_steps = time_steps;
    result.depth_levels_per_side = depth;
    result.features = features;
    result.data.resize(time_steps * depth * 2 * features);

    std::mt19937 gen(params.random_seed);
    std::normal_distribution<float> price_change(0.0f, static_cast<float>(params.base_price * params.price_volatility));
    std::cauchy_distribution<float> large_move(0.0f, static_cast<float>(params.base_price * 0.002f));
    std::bernoulli_distribution large_move_prob(0.05);

    float current_mid = static_cast<float>(params.base_price);
    
    for (size_t t = 0; t < time_steps; ++t) {
        float price_move = price_change(gen);
        if (large_move_prob(gen)) price_move += large_move(gen);
        current_mid += price_move;

        float spread = static_cast<float>(params.spread_pct * current_mid);
        float best_bid = current_mid - spread / 2.0f;
        float best_ask = current_mid + spread / 2.0f;

        for (size_t l = 0; l < depth; ++l) {
            size_t bid_base_idx = (t * depth * 2 + l) * features;
            size_t ask_base_idx = (t * depth * 2 + depth + l) * features;

            // Prices
            float bid_price = best_bid - l * static_cast<float>(params.tick_size);
            float ask_price = best_ask + l * static_cast<float>(params.tick_size);

            // Volume (simple U-shape)
            float norm_level = static_cast<float>(l) / (depth - 1);
            float vol_factor = 1.0f + 4.0f * std::pow(norm_level - 0.5f, 2.0f);
            float bid_volume = static_cast<float>(params.base_volume) * vol_factor;
            float ask_volume = static_cast<float>(params.base_volume) * vol_factor * 1.1f; // Skew
            
            // Count
            float bid_count = std::max(1.0f, bid_volume * static_cast<float>(params.count_to_volume_ratio));
            float ask_count = std::max(1.0f, ask_volume * static_cast<float>(params.count_to_volume_ratio));
            
            // --- Assign to Flat Vector ---
            result.data[bid_base_idx + 0] = bid_price;
            result.data[bid_base_idx + 1] = bid_volume;
            result.data[bid_base_idx + 2] = bid_count;
            result.data[ask_base_idx + 0] = ask_price;
            result.data[ask_base_idx + 1] = ask_volume;
            result.data[ask_base_idx + 2] = ask_count;
        }
    }

    return result;
}


} // namespace test_helpers