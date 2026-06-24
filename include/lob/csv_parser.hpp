#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <cstddef>

#include "lob/event.hpp"
#include "lob/fixed_matching_book.hpp"
#include "lob/order.hpp"

namespace lob {

class CsvParser {
public:
    using OrderHandler = std::function<void(const Order&)>;

    struct ReplayStats {
        std::uint64_t orders_processed {};
        std::uint64_t generated_trades {};
        std::uint64_t first_timestamp {};
        std::uint64_t last_timestamp {};
    };

    CsvParser() = default;
    explicit CsvParser(const std::filesystem::path& file_path);
    ~CsvParser();

    CsvParser(const CsvParser&) = delete;
    CsvParser& operator=(const CsvParser&) = delete;
    CsvParser(CsvParser&& other) noexcept;
    CsvParser& operator=(CsvParser&& other) noexcept;

    [[nodiscard]] bool has_next() const noexcept {
        return has_next_;
    }

    [[nodiscard]] std::uint64_t peek_time() const noexcept {
        return next_event_.timestamp;
    }

    [[nodiscard]] Event pop();

    [[nodiscard]] std::uint64_t parse_file(const std::filesystem::path& file_path, const OrderHandler& handler) const;
    [[nodiscard]] std::uint64_t process_file(
        const std::filesystem::path& file_path,
        FixedMatchingBook& book
    ) const;
    [[nodiscard]] ReplayStats replay_file(
        const std::filesystem::path& file_path,
        FixedMatchingBook& book
    ) const;

private:
    void map_file(const std::filesystem::path& file_path);
    void close_mapping() noexcept;
    void parse_next();

    int file_descriptor_ {-1};
    void* mapping_ {nullptr};
    std::size_t size_ {};
    const char* cursor_ {nullptr};
    const char* end_ {nullptr};
    Event next_event_ {};
    bool has_next_ {};
};

}  // namespace lob
