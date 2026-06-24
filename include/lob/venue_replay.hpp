#pragma once

#include <array>
#include <cstdint>

namespace lob {

using VenueID = std::uint16_t;
using ProductID = std::uint32_t;

enum class UpdateSemantics : std::uint8_t {
    Absolute,
    Delta,
};

enum class ReplayGapPolicy : std::uint8_t {
    Reject,
    Quarantine,
    Ignore,
};

enum class ReplayValidationStatus : std::uint8_t {
    Accepted,
    SequenceGap,
    SequenceReorder,
    SnapshotEpochRegression,
    UnsupportedAddress,
};

struct FeedEnvelope {
    VenueID venue_id {};
    ProductID product_id {};
    std::uint64_t exchange_ts_ns {};
    std::uint64_t receive_ts_ns {};
    std::uint64_t sequence {};
    std::uint64_t snapshot_epoch {};
    std::uint32_t checksum {};
    UpdateSemantics semantics {UpdateSemantics::Absolute};
};

struct NormalizedBookUpdate {
    FeedEnvelope envelope {};
    bool is_bid {};
    bool is_snapshot {};
    std::int64_t price_ticks {};
    std::int64_t qty_lots {};
};

class ReplaySequenceValidator {
public:
    explicit ReplaySequenceValidator(ReplayGapPolicy gap_policy = ReplayGapPolicy::Reject)
        : gap_policy_(gap_policy) {}

    [[nodiscard]] ReplayValidationStatus validate(const FeedEnvelope& envelope) {
        if (envelope.venue_id >= kMaxVenues || envelope.product_id >= kMaxProducts) {
            return ReplayValidationStatus::UnsupportedAddress;
        }

        ProductState& state = product_states_[index(envelope.venue_id, envelope.product_id)];
        if (!state.initialized) {
            state.initialized = true;
            state.last_sequence = envelope.sequence;
            state.snapshot_epoch = envelope.snapshot_epoch;
            return ReplayValidationStatus::Accepted;
        }

        if (envelope.snapshot_epoch < state.snapshot_epoch) {
            return ReplayValidationStatus::SnapshotEpochRegression;
        }
        if (envelope.snapshot_epoch > state.snapshot_epoch) {
            state.snapshot_epoch = envelope.snapshot_epoch;
            state.last_sequence = envelope.sequence;
            return ReplayValidationStatus::Accepted;
        }
        if (envelope.sequence <= state.last_sequence) {
            return ReplayValidationStatus::SequenceReorder;
        }
        if (envelope.sequence != state.last_sequence + 1U) {
            if (gap_policy_ == ReplayGapPolicy::Ignore) {
                state.last_sequence = envelope.sequence;
                return ReplayValidationStatus::Accepted;
            }
            return ReplayValidationStatus::SequenceGap;
        }

        state.last_sequence = envelope.sequence;
        return ReplayValidationStatus::Accepted;
    }

    void reset() {
        product_states_.fill(ProductState {});
    }

private:
    struct ProductState {
        std::uint64_t last_sequence {};
        std::uint64_t snapshot_epoch {};
        bool initialized {};
    };

    static constexpr std::size_t kMaxVenues = 16U;
    static constexpr std::size_t kMaxProducts = 256U;

    [[nodiscard]] static constexpr std::size_t index(VenueID venue_id, ProductID product_id) noexcept {
        return static_cast<std::size_t>(venue_id) * kMaxProducts + static_cast<std::size_t>(product_id);
    }

    ReplayGapPolicy gap_policy_ {ReplayGapPolicy::Reject};
    std::array<ProductState, kMaxVenues * kMaxProducts> product_states_ {};
};

}  // namespace lob
