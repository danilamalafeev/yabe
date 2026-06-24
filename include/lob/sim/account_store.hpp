#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

#include "lob/event.hpp"
#include "lob/sim/types.hpp"

namespace lob::sim {

using AccountID = AgentID;
using QtyLots = std::uint64_t;
using PriceTicks = std::int64_t;
using BalanceLots = std::int64_t;

struct AccountBalance {
    BalanceLots available_lots {};
    BalanceLots reserved_lots {};

    [[nodiscard]] friend constexpr bool operator==(AccountBalance lhs, AccountBalance rhs) noexcept = default;
};

struct AccountSnapshot {
    AgentID agent_id {invalid_agent_id};
    AssetID asset_id {};
    AccountBalance balance {};
};

template <std::size_t AccountCapacity, std::size_t AssetCapacity>
class AccountStore {
public:
    [[nodiscard]] bool register_account(AgentID agent_id) noexcept {
        if (agent_id == invalid_agent_id) {
            return false;
        }
        if (find_account_index(agent_id).has_value()) {
            return true;
        }
        if (account_count_ == AccountCapacity) {
            return false;
        }

        accounts_[account_count_] = Account {
            .agent_id = agent_id,
            .active = true,
            .balances = {},
        };
        ++account_count_;
        return true;
    }

    [[nodiscard]] bool has_account(AgentID agent_id) const noexcept {
        return find_account_index(agent_id).has_value();
    }

    [[nodiscard]] bool set_balance(AgentID agent_id, AssetID asset_id, BalanceLots amount_lots) noexcept {
        Account* account = find_account(agent_id);
        if (account == nullptr || !valid_asset(asset_id)) {
            return false;
        }

        account->balances[asset_id] = AccountBalance {
            .available_lots = amount_lots,
            .reserved_lots = 0,
        };
        return true;
    }

    [[nodiscard]] std::optional<AccountBalance> balance(AgentID agent_id, AssetID asset_id) const noexcept {
        const Account* account = find_account(agent_id);
        if (account == nullptr || !valid_asset(asset_id)) {
            return std::nullopt;
        }
        return account->balances[asset_id];
    }

    [[nodiscard]] bool reserve(AgentID agent_id, AssetID asset_id, BalanceLots amount_lots) noexcept {
        Account* account = find_account(agent_id);
        if (account == nullptr || !valid_asset(asset_id) || amount_lots < 0) {
            return false;
        }

        AccountBalance& bal = account->balances[asset_id];
        if (bal.available_lots < amount_lots) {
            return false;
        }
        if (add_overflows(bal.reserved_lots, amount_lots)) {
            return false;
        }

        bal.available_lots -= amount_lots;
        bal.reserved_lots += amount_lots;
        return true;
    }

    [[nodiscard]] bool release(AgentID agent_id, AssetID asset_id, BalanceLots amount_lots) noexcept {
        Account* account = find_account(agent_id);
        if (account == nullptr || !valid_asset(asset_id) || amount_lots < 0) {
            return false;
        }

        AccountBalance& bal = account->balances[asset_id];
        if (bal.reserved_lots < amount_lots) {
            return false;
        }
        if (add_overflows(bal.available_lots, amount_lots)) {
            return false;
        }

        bal.reserved_lots -= amount_lots;
        bal.available_lots += amount_lots;
        return true;
    }

    [[nodiscard]] bool consume_reserved(AgentID agent_id, AssetID asset_id, BalanceLots amount_lots) noexcept {
        Account* account = find_account(agent_id);
        if (account == nullptr || !valid_asset(asset_id) || amount_lots < 0) {
            return false;
        }

        AccountBalance& bal = account->balances[asset_id];
        if (bal.reserved_lots < amount_lots) {
            return false;
        }

        bal.reserved_lots -= amount_lots;
        return true;
    }

    [[nodiscard]] bool apply_delta(AgentID agent_id, AssetID asset_id, BalanceLots amount_lots) noexcept {
        Account* account = find_account(agent_id);
        if (account == nullptr || !valid_asset(asset_id)) {
            return false;
        }
        if (add_overflows(account->balances[asset_id].available_lots, amount_lots)) {
            return false;
        }

        account->balances[asset_id].available_lots += amount_lots;
        return true;
    }

    [[nodiscard]] std::size_t account_count() const noexcept {
        return account_count_;
    }

#ifdef LOB_SIM_TESTING
    [[nodiscard]] std::size_t account_snapshot_count() const noexcept {
        return account_count_ * AssetCapacity;
    }

    [[nodiscard]] std::optional<AccountSnapshot> account_snapshot(std::size_t index) const noexcept {
        if (index >= account_snapshot_count()) {
            return std::nullopt;
        }

        const std::size_t account_index = index / AssetCapacity;
        const AssetID asset_id = static_cast<AssetID>(index % AssetCapacity);
        const Account& account = accounts_[account_index];
        return AccountSnapshot {
            .agent_id = account.agent_id,
            .asset_id = asset_id,
            .balance = account.balances[asset_id],
        };
    }
#endif

private:
    struct Account {
        AgentID agent_id {invalid_agent_id};
        bool active {};
        std::array<AccountBalance, AssetCapacity> balances {};
    };

    [[nodiscard]] static constexpr bool valid_asset(AssetID asset_id) noexcept {
        return static_cast<std::size_t>(asset_id) < AssetCapacity;
    }

    [[nodiscard]] static constexpr bool add_overflows(BalanceLots lhs, BalanceLots rhs) noexcept {
        if (rhs > 0 && lhs > std::numeric_limits<BalanceLots>::max() - rhs) {
            return true;
        }
        if (rhs < 0 && lhs < std::numeric_limits<BalanceLots>::min() - rhs) {
            return true;
        }
        return false;
    }

    [[nodiscard]] std::optional<std::size_t> find_account_index(AgentID agent_id) const noexcept {
        for (std::size_t i = 0U; i < account_count_; ++i) {
            if (accounts_[i].active && accounts_[i].agent_id == agent_id) {
                return i;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] Account* find_account(AgentID agent_id) noexcept {
        const std::optional<std::size_t> index = find_account_index(agent_id);
        if (!index.has_value()) {
            return nullptr;
        }
        return &accounts_[*index];
    }

    [[nodiscard]] const Account* find_account(AgentID agent_id) const noexcept {
        const std::optional<std::size_t> index = find_account_index(agent_id);
        if (!index.has_value()) {
            return nullptr;
        }
        return &accounts_[*index];
    }

    std::array<Account, AccountCapacity> accounts_ {};
    std::size_t account_count_ {};
};

}  // namespace lob::sim
