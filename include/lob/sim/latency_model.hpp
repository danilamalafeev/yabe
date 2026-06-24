#pragma once

#include <cmath>
#include <cstdint>
#include <limits>

#include "lob/sim/types.hpp"

namespace lob::sim {

enum class LatencyDistribution : std::uint8_t {
    Fixed,
    Uniform,
    Exponential,
    LogNormal,
};

class LatencyModel {
public:
    constexpr LatencyModel() noexcept = default;

    explicit constexpr LatencyModel(TimeNs base_latency_ns, TimeNs jitter_bound_ns = 0U, std::uint64_t seed = 1U) noexcept
        : base_latency_ns_(base_latency_ns),
          jitter_bound_ns_(jitter_bound_ns),
          distribution_(jitter_bound_ns == 0U ? LatencyDistribution::Fixed : LatencyDistribution::Uniform),
          rng_state_(seed == 0U ? 1U : seed) {}

    explicit constexpr LatencyModel(LatencyDistribution distribution,
                                    TimeNs base_latency_ns,
                                    TimeNs jitter_bound_ns = 0U,
                                    std::uint64_t seed = 1U) noexcept
        : base_latency_ns_(base_latency_ns),
          jitter_bound_ns_(jitter_bound_ns),
          exponential_mean_ns_(distribution == LatencyDistribution::Exponential ? static_cast<double>(jitter_bound_ns) : 0.0),
          distribution_(distribution),
          rng_state_(seed == 0U ? 1U : seed) {}

    [[nodiscard]] static constexpr LatencyModel fixed(TimeNs base_latency_ns, std::uint64_t seed = 1U) noexcept {
        return LatencyModel {LatencyDistribution::Fixed, base_latency_ns, 0U, 0.0, 0.0, 0.0, seed};
    }

    [[nodiscard]] static constexpr LatencyModel uniform(TimeNs base_latency_ns,
                                                        TimeNs jitter_bound_ns,
                                                        std::uint64_t seed = 1U) noexcept {
        return LatencyModel {LatencyDistribution::Uniform, base_latency_ns, jitter_bound_ns, 0.0, 0.0, 0.0, seed};
    }

    [[nodiscard]] static constexpr LatencyModel exponential(TimeNs base_latency_ns,
                                                            double exponential_mean_ns,
                                                            std::uint64_t seed = 1U) noexcept {
        return LatencyModel {
            LatencyDistribution::Exponential, base_latency_ns, 0U, exponential_mean_ns, 0.0, 0.0, seed};
    }

    [[nodiscard]] static constexpr LatencyModel lognormal(TimeNs base_latency_ns,
                                                          double lognormal_mu,
                                                          double lognormal_sigma,
                                                          std::uint64_t seed = 1U) noexcept {
        return LatencyModel {
            LatencyDistribution::LogNormal, base_latency_ns, 0U, 0.0, lognormal_mu, lognormal_sigma, seed};
    }

    [[nodiscard]] TimeNs next_latency_ns() noexcept {
        if (!valid_config()) {
            return base_latency_ns_;
        }

        TimeNs jitter {};
        switch (distribution_) {
        case LatencyDistribution::Fixed:
            jitter = 0U;
            break;
        case LatencyDistribution::Uniform:
            jitter = sample_uniform_jitter();
            break;
        case LatencyDistribution::Exponential:
            jitter = sample_exponential_jitter();
            break;
        case LatencyDistribution::LogNormal:
            jitter = sample_lognormal_jitter();
            break;
        }
        if (std::numeric_limits<TimeNs>::max() - base_latency_ns_ < jitter) {
            return std::numeric_limits<TimeNs>::max();
        }
        return base_latency_ns_ + jitter;
    }

    void set_base_latency_ns(TimeNs base_latency_ns) noexcept {
        base_latency_ns_ = base_latency_ns;
    }

    void set_jitter_bound_ns(TimeNs jitter_bound_ns) noexcept {
        jitter_bound_ns_ = jitter_bound_ns;
        distribution_ = jitter_bound_ns == 0U ? LatencyDistribution::Fixed : LatencyDistribution::Uniform;
    }

    void set_distribution(LatencyDistribution distribution) noexcept {
        distribution_ = distribution;
    }

    void set_exponential_mean_ns(double exponential_mean_ns) noexcept {
        exponential_mean_ns_ = exponential_mean_ns;
    }

    void set_lognormal_mu(double lognormal_mu) noexcept {
        lognormal_mu_ = lognormal_mu;
    }

    void set_lognormal_sigma(double lognormal_sigma) noexcept {
        lognormal_sigma_ = lognormal_sigma;
    }

    void seed(std::uint64_t seed_value) noexcept {
        rng_state_ = seed_value == 0U ? 1U : seed_value;
    }

    [[nodiscard]] TimeNs base_latency_ns() const noexcept {
        return base_latency_ns_;
    }

    [[nodiscard]] TimeNs jitter_bound_ns() const noexcept {
        return jitter_bound_ns_;
    }

    [[nodiscard]] LatencyDistribution distribution() const noexcept {
        return distribution_;
    }

    [[nodiscard]] double exponential_mean_ns() const noexcept {
        return exponential_mean_ns_;
    }

    [[nodiscard]] double lognormal_mu() const noexcept {
        return lognormal_mu_;
    }

    [[nodiscard]] double lognormal_sigma() const noexcept {
        return lognormal_sigma_;
    }

    [[nodiscard]] bool valid_config() const noexcept {
        switch (distribution_) {
        case LatencyDistribution::Fixed:
            return true;
        case LatencyDistribution::Uniform:
            return true;
        case LatencyDistribution::Exponential:
            return exponential_mean_ns_ > 0.0 && std::isfinite(exponential_mean_ns_);
        case LatencyDistribution::LogNormal:
            return std::isfinite(lognormal_mu_) && lognormal_sigma_ > 0.0 && std::isfinite(lognormal_sigma_);
        }
        return false;
    }

private:
    constexpr LatencyModel(LatencyDistribution distribution,
                           TimeNs base_latency_ns,
                           TimeNs jitter_bound_ns,
                           double exponential_mean_ns,
                           double lognormal_mu,
                           double lognormal_sigma,
                           std::uint64_t seed) noexcept
        : base_latency_ns_(base_latency_ns),
          jitter_bound_ns_(jitter_bound_ns),
          exponential_mean_ns_(exponential_mean_ns),
          lognormal_mu_(lognormal_mu),
          lognormal_sigma_(lognormal_sigma),
          distribution_(distribution),
          rng_state_(seed == 0U ? 1U : seed) {}

    [[nodiscard]] std::uint64_t next_random() noexcept {
        std::uint64_t x = rng_state_;
        x ^= x << 13U;
        x ^= x >> 7U;
        x ^= x << 17U;
        rng_state_ = x == 0U ? 1U : x;
        return rng_state_;
    }

    [[nodiscard]] TimeNs sample_uniform_jitter() noexcept {
        if (jitter_bound_ns_ == 0U) {
            return 0U;
        }
        return jitter_bound_ns_ == std::numeric_limits<TimeNs>::max() ? next_random()
                                                                      : next_random() % (jitter_bound_ns_ + 1U);
    }

    [[nodiscard]] double next_unit_open() noexcept {
        constexpr double scale = 1.0 / static_cast<double>(std::numeric_limits<std::uint64_t>::max());
        const double unit = (static_cast<double>(next_random()) + 0.5) * scale;
        if (unit <= 0.0) {
            return std::numeric_limits<double>::min();
        }
        if (unit >= 1.0) {
            return std::nextafter(1.0, 0.0);
        }
        return unit;
    }

    [[nodiscard]] static TimeNs double_to_latency_ns(double value) noexcept {
        if (!(value > 0.0) || !std::isfinite(value)) {
            return 0U;
        }
        const auto max_time_ns = static_cast<double>(std::numeric_limits<TimeNs>::max());
        if (value >= max_time_ns) {
            return std::numeric_limits<TimeNs>::max();
        }
        return static_cast<TimeNs>(value);
    }

    [[nodiscard]] TimeNs sample_exponential_jitter() noexcept {
        const double unit = next_unit_open();
        return double_to_latency_ns(-exponential_mean_ns_ * std::log1p(-unit));
    }

    [[nodiscard]] TimeNs sample_lognormal_jitter() noexcept {
        constexpr double two_pi = 6.283185307179586476925286766559;
        const double u1 = next_unit_open();
        const double u2 = next_unit_open();
        const double z = std::sqrt(-2.0 * std::log(u1)) * std::cos(two_pi * u2);
        return double_to_latency_ns(std::exp(lognormal_mu_ + (lognormal_sigma_ * z)));
    }

    TimeNs base_latency_ns_ {};
    TimeNs jitter_bound_ns_ {};
    double exponential_mean_ns_ {};
    double lognormal_mu_ {};
    double lognormal_sigma_ {};
    LatencyDistribution distribution_ {LatencyDistribution::Fixed};
    std::uint64_t rng_state_ {1U};
};

}  // namespace lob::sim
