#include "lob/csv_parser.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>


namespace lob {
namespace {

constexpr std::uint64_t kQuantityScale = 100'000'000U;

[[nodiscard]] inline bool IsDigit(char value) noexcept {
    return value >= '0' && value <= '9';
}

[[nodiscard]] inline std::uint64_t ParseUnsignedField(const char*& cursor, const char* end) {
    std::uint64_t value = 0U;
    while (cursor < end && IsDigit(*cursor)) {
        value = (value * 10U) + static_cast<std::uint64_t>(*cursor - '0');
        ++cursor;
    }
    return value;
}

[[nodiscard]] inline double ParseDoubleField(const char*& cursor, const char* end) {
    std::uint64_t integer_part = 0U;
    while (cursor < end && IsDigit(*cursor)) {
        integer_part = (integer_part * 10U) + static_cast<std::uint64_t>(*cursor - '0');
        ++cursor;
    }

    std::uint64_t fractional_part = 0U;
    std::uint64_t fractional_scale = 1U;
    if (cursor < end && *cursor == '.') {
        ++cursor;

        while (cursor < end && IsDigit(*cursor)) {
            fractional_part = (fractional_part * 10U) + static_cast<std::uint64_t>(*cursor - '0');
            fractional_scale *= 10U;
            ++cursor;
        }
    }

    return static_cast<double>(integer_part) +
           (static_cast<double>(fractional_part) / static_cast<double>(fractional_scale));
}

[[nodiscard]] inline std::uint64_t ParseScaledQuantityField(const char*& cursor, const char* end) {
    std::uint64_t integer_part = 0U;
    while (cursor < end && IsDigit(*cursor)) {
        integer_part = (integer_part * 10U) + static_cast<std::uint64_t>(*cursor - '0');
        ++cursor;
    }

    std::uint64_t scaled_quantity = integer_part * kQuantityScale;
    if (cursor < end && *cursor == '.') {
        ++cursor;

        std::uint64_t multiplier = kQuantityScale / 10U;
        while (cursor < end && IsDigit(*cursor) && multiplier > 0U) {
            scaled_quantity += static_cast<std::uint64_t>(*cursor - '0') * multiplier;
            multiplier /= 10U;
            ++cursor;
        }

        while (cursor < end && IsDigit(*cursor)) {
            ++cursor;
        }
    }

    return scaled_quantity;
}

[[nodiscard]] inline std::int64_t ParseScaledPriceField(const char*& cursor, const char* end) {
    std::uint64_t integer_part = 0U;
    while (cursor < end && IsDigit(*cursor)) {
        integer_part = (integer_part * 10U) + static_cast<std::uint64_t>(*cursor - '0');
        ++cursor;
    }

    std::uint64_t scaled_price = integer_part * 100'000'000ULL;
    if (cursor < end && *cursor == '.') {
        ++cursor;

        std::uint64_t multiplier = 10'000'000ULL;
        while (cursor < end && IsDigit(*cursor) && multiplier > 0U) {
            scaled_price += static_cast<std::uint64_t>(*cursor - '0') * multiplier;
            multiplier /= 10U;
            ++cursor;
        }

        while (cursor < end && IsDigit(*cursor)) {
            ++cursor;
        }
    }

    return static_cast<std::int64_t>(scaled_price);
}

[[nodiscard]] inline Side ParseIncomingSideField(const char*& cursor, const char* end) {
    if (cursor >= end) {
        throw std::runtime_error("Unexpected end of file while parsing side");
    }

    const char first_character = *cursor;
    while (cursor < end && *cursor != ',' && *cursor != '\n' && *cursor != '\r') {
        ++cursor;
    }

    if (first_character == 't' || first_character == 'T' || first_character == '1') {
        return Side::Sell;
    }

    if (first_character == 'f' || first_character == 'F' || first_character == '0') {
        return Side::Buy;
    }

    throw std::runtime_error("Invalid is_buyer_maker field");
}

inline void ExpectComma(const char*& cursor, const char* end) {
    if (cursor >= end || *cursor != ',') {
        throw std::runtime_error("Malformed CSV row: expected comma delimiter");
    }
    ++cursor;
}

inline void SkipField(const char*& cursor, const char* end) noexcept {
    while (cursor < end && *cursor != ',' && *cursor != '\n' && *cursor != '\r') {
        ++cursor;
    }
}

inline void ConsumeRecordEnd(const char*& cursor, const char* end) {
    if (cursor < end && *cursor == '\r') {
        ++cursor;
    }

    if (cursor < end && *cursor == '\n') {
        ++cursor;
    } else if (cursor < end) {
        throw std::runtime_error("Malformed CSV row: expected newline");
    }
}

inline void SkipEmptyLine(const char*& cursor, const char* end) noexcept {
    if (cursor < end && *cursor == '\r') {
        ++cursor;
    }

    if (cursor < end && *cursor == '\n') {
        ++cursor;
    }
}

}  // namespace

CsvParser::CsvParser(const std::filesystem::path& file_path) {
    map_file(file_path);
    parse_next();
}

CsvParser::~CsvParser() {
    close_mapping();
}

CsvParser::CsvParser(CsvParser&& other) noexcept
    : file_descriptor_(other.file_descriptor_),
      mapping_(other.mapping_),
      size_(other.size_),
      cursor_(other.cursor_),
      end_(other.end_),
      next_event_(other.next_event_),
      has_next_(other.has_next_) {
    other.file_descriptor_ = -1;
    other.mapping_ = nullptr;
    other.size_ = 0U;
    other.cursor_ = nullptr;
    other.end_ = nullptr;
    other.next_event_ = Event {};
    other.has_next_ = false;
}

CsvParser& CsvParser::operator=(CsvParser&& other) noexcept {
    if (this == &other) {
        return *this;
    }

    close_mapping();

    file_descriptor_ = other.file_descriptor_;
    mapping_ = other.mapping_;
    size_ = other.size_;
    cursor_ = other.cursor_;
    end_ = other.end_;
    next_event_ = other.next_event_;
    has_next_ = other.has_next_;

    other.file_descriptor_ = -1;
    other.mapping_ = nullptr;
    other.size_ = 0U;
    other.cursor_ = nullptr;
    other.end_ = nullptr;
    other.next_event_ = Event {};
    other.has_next_ = false;
    return *this;
}

Event CsvParser::pop() {
    if (!has_next_) {
        throw std::out_of_range("CsvParser::pop called with no remaining events");
    }

    Event event = next_event_;
    parse_next();
    return event;
}

void CsvParser::map_file(const std::filesystem::path& file_path) {
    file_descriptor_ = ::open(file_path.c_str(), O_RDONLY);
    if (file_descriptor_ == -1) {
        throw std::runtime_error("Unable to open CSV file: " + file_path.string());
    }

    struct stat file_stat {};
    if (::fstat(file_descriptor_, &file_stat) == -1) {
        const int saved_errno = errno;
        close_mapping();
        throw std::runtime_error("Unable to stat CSV file: " + std::to_string(saved_errno));
    }

    size_ = static_cast<std::size_t>(file_stat.st_size);
    if (size_ == 0U) {
        cursor_ = nullptr;
        end_ = nullptr;
        return;
    }

    mapping_ = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, file_descriptor_, 0);
    if (mapping_ == MAP_FAILED) {
        const int saved_errno = errno;
        mapping_ = nullptr;
        close_mapping();
        throw std::runtime_error("Unable to mmap CSV file: " + std::to_string(saved_errno));
    }

    cursor_ = static_cast<const char*>(mapping_);
    end_ = cursor_ + size_;
    (void)::madvise(mapping_, size_, MADV_SEQUENTIAL);
}

void CsvParser::close_mapping() noexcept {
    if (mapping_ != nullptr) {
        ::munmap(mapping_, size_);
        mapping_ = nullptr;
    }

    if (file_descriptor_ != -1) {
        ::close(file_descriptor_);
        file_descriptor_ = -1;
    }

    size_ = 0U;
    cursor_ = nullptr;
    end_ = nullptr;
    has_next_ = false;
}

void CsvParser::parse_next() {
    has_next_ = false;

    while (cursor_ != nullptr && cursor_ < end_) {
        if (*cursor_ == '\n' || *cursor_ == '\r') {
            SkipEmptyLine(cursor_, end_);
            continue;
        }

        Order order {};
        order.id = ParseUnsignedField(cursor_, end_);
        ExpectComma(cursor_, end_);
        order.price = ParseScaledPriceField(cursor_, end_);
        ExpectComma(cursor_, end_);
        order.quantity = ParseScaledQuantityField(cursor_, end_);
        ExpectComma(cursor_, end_);
        SkipField(cursor_, end_);
        ExpectComma(cursor_, end_);
        order.timestamp = ParseUnsignedField(cursor_, end_);
        ExpectComma(cursor_, end_);
        order.side = ParseIncomingSideField(cursor_, end_);
        ExpectComma(cursor_, end_);
        SkipField(cursor_, end_);
        ConsumeRecordEnd(cursor_, end_);

        next_event_ = Event {
            .timestamp = order.timestamp,
            .order = order,
        };
        has_next_ = true;
        return;
    }
}

std::uint64_t CsvParser::parse_file(const std::filesystem::path& file_path, const OrderHandler& handler) const {
    if (!handler) {
        throw std::invalid_argument("CSV parser requires a valid order handler");
    }

    CsvParser parser {file_path};
    std::uint64_t parsed_orders = 0U;
    while (parser.has_next()) {
        handler(parser.pop().order);
        ++parsed_orders;
    }

    return parsed_orders;
}

std::uint64_t CsvParser::process_file(const std::filesystem::path& file_path, FixedMatchingBook& book) const {
    CsvParser parser {file_path};
    std::uint64_t processed_orders = 0U;
    while (parser.has_next()) {
        (void)book.submit(parser.pop().order);
        ++processed_orders;
    }
    return processed_orders;
}

CsvParser::ReplayStats CsvParser::replay_file(const std::filesystem::path& file_path, FixedMatchingBook& book) const {
    ReplayStats replay_stats {};
    CsvParser parser {file_path};

    while (parser.has_next()) {
        const Order order = parser.pop().order;
        if (replay_stats.first_timestamp == 0U) {
            replay_stats.first_timestamp = order.timestamp;
        }

        replay_stats.last_timestamp = order.timestamp;
        ++replay_stats.orders_processed;
        replay_stats.generated_trades += static_cast<std::uint64_t>(book.submit(order).execution_count / 2U);
    }

    return replay_stats;
}

}  // namespace lob
