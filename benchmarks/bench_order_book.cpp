#include <benchmark/benchmark.h>

#include <cstdint>
#include <memory>

#include "lob/fixed_matching_book.hpp"

namespace {

void Seed(
    lob::FixedMatchingBook& book,
    lob::Side side,
    std::int64_t count,
    std::uint64_t starting_id
) {
    for (std::int64_t index = 0; index < count; ++index) {
        const std::int64_t price = side == lob::Side::Buy
            ? lob::FixedMatchingBook::price_to_ticks(1'000.0 - static_cast<double>(index) * 0.01)
            : lob::FixedMatchingBook::price_to_ticks(1'000.0 + static_cast<double>(index) * 0.01);
        benchmark::DoNotOptimize(book.submit(
            starting_id + static_cast<std::uint64_t>(index),
            side,
            price,
            10U,
            static_cast<std::uint64_t>(index)
        ));
    }
}

void BM_FixedMatchingRestingInsert(benchmark::State& state) {
    std::uint64_t next_order_id = 1'000'000U;
    for (auto _ : state) {
        state.PauseTiming();
        auto book = std::make_unique<lob::FixedMatchingBook>();
        Seed(*book, lob::Side::Buy, state.range(0), 1U);
        const std::uint64_t order_id = next_order_id++;
        state.ResumeTiming();
        benchmark::DoNotOptimize(book->submit(
            order_id,
            lob::Side::Buy,
            lob::FixedMatchingBook::price_to_ticks(2'000.0),
            10U,
            order_id
        ));
        benchmark::ClobberMemory();
    }
}

void BM_FixedMatchingAggressiveMatch(benchmark::State& state) {
    std::uint64_t next_order_id = 2'000'000U;
    for (auto _ : state) {
        state.PauseTiming();
        auto book = std::make_unique<lob::FixedMatchingBook>();
        Seed(*book, lob::Side::Sell, state.range(0), 1U);
        const std::uint64_t order_id = next_order_id++;
        state.ResumeTiming();
        benchmark::DoNotOptimize(book->submit(
            order_id,
            lob::Side::Buy,
            lob::FixedMatchingBook::price_to_ticks(2'000.0),
            10U,
            order_id
        ));
        benchmark::ClobberMemory();
    }
}

void BM_FixedMatchingCancel(benchmark::State& state) {
    std::uint64_t next_order_id = 3'000'000U;
    for (auto _ : state) {
        state.PauseTiming();
        auto book = std::make_unique<lob::FixedMatchingBook>();
        Seed(*book, lob::Side::Buy, state.range(0), 1U);
        const std::uint64_t order_id = next_order_id++;
        benchmark::DoNotOptimize(book->submit(
            order_id,
            lob::Side::Buy,
            lob::FixedMatchingBook::price_to_ticks(500.0),
            10U,
            order_id
        ));
        state.ResumeTiming();
        benchmark::DoNotOptimize(book->cancel(order_id));
        benchmark::ClobberMemory();
    }
}

BENCHMARK(BM_FixedMatchingRestingInsert)->Arg(100)->Arg(1'000)->Arg(5'000);
BENCHMARK(BM_FixedMatchingAggressiveMatch)->Arg(100)->Arg(1'000)->Arg(5'000);
BENCHMARK(BM_FixedMatchingCancel)->Arg(100)->Arg(1'000)->Arg(5'000);

}  // namespace
