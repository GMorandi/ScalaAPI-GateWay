#include <benchmark/benchmark.h>
#include "usage/usage_collector.h"

using namespace gateway::usage;

static UsageEvent make_event(int i) {
    UsageEvent e;
    e.lease_token = "lease-" + std::to_string(i);
    e.request_id = "req-" + std::to_string(i);
    e.api_key_id = i;
    e.user_id = i % 100;
    e.account_id = i % 10;
    e.group_id = 1;
    e.model = "gpt-4o";
    e.upstream_model = "gpt-4o-2024-08-06";
    e.input_tokens = 512;
    e.output_tokens = 256;
    e.duration_ms = 1200;
    e.first_token_ms = 300;
    e.stream = true;
    return e;
}

static void BM_RecordSingle(benchmark::State& state) {
    UsageCollector collector;
    int i = 0;
    for (auto _ : state) {
        collector.record(make_event(i++));
        if (collector.pending() >= 4000) collector.drain();
    }
}
BENCHMARK(BM_RecordSingle);

static void BM_RecordAndDrain4096(benchmark::State& state) {
    UsageCollector collector;
    for (auto _ : state) {
        state.PauseTiming();
        for (int i = 0; i < 4096; ++i) {
            collector.record(make_event(i));
        }
        state.ResumeTiming();
        auto events = collector.drain();
        benchmark::DoNotOptimize(events);
    }
    state.SetItemsProcessed(state.iterations() * 4096);
}
BENCHMARK(BM_RecordAndDrain4096);

static void BM_DrainEmpty(benchmark::State& state) {
    UsageCollector collector;
    for (auto _ : state) {
        auto events = collector.drain();
        benchmark::DoNotOptimize(events);
    }
}
BENCHMARK(BM_DrainEmpty);

static void BM_OverflowStress(benchmark::State& state) {
    UsageCollector collector;
    for (auto _ : state) {
        for (int i = 0; i < 8192; ++i) {
            collector.record(make_event(i));
        }
        auto events = collector.drain();
        benchmark::DoNotOptimize(events);
    }
    state.SetItemsProcessed(state.iterations() * 8192);
}
BENCHMARK(BM_OverflowStress);
