#include <gtest/gtest.h>
#include "usage/usage_collector.h"
#include <filesystem>
#include <unistd.h>

using namespace gateway::usage;

static UsageEvent make_event(int id) {
    UsageEvent ev;
    ev.api_key_id = id;
    ev.request_id = "req-" + std::to_string(id);
    ev.input_tokens = id * 10;
    ev.output_tokens = id * 5;
    return ev;
}

TEST(UsageCollector, InitiallyEmpty) {
    UsageCollector collector;
    EXPECT_EQ(collector.pending(), 0u);
    auto events = collector.drain();
    EXPECT_TRUE(events.empty());
}

TEST(UsageCollector, RecordAndPending) {
    UsageCollector collector;
    collector.record(make_event(1));
    collector.record(make_event(2));
    collector.record(make_event(3));
    EXPECT_EQ(collector.pending(), 3u);
}

TEST(UsageCollector, DrainReturnsFIFO) {
    UsageCollector collector;
    collector.record(make_event(10));
    collector.record(make_event(20));
    collector.record(make_event(30));

    auto events = collector.drain();
    ASSERT_EQ(events.size(), 3u);
    EXPECT_EQ(events[0].api_key_id, 10);
    EXPECT_EQ(events[1].api_key_id, 20);
    EXPECT_EQ(events[2].api_key_id, 30);
}

TEST(UsageCollector, DrainResetsCount) {
    UsageCollector collector;
    collector.record(make_event(1));
    collector.record(make_event(2));
    collector.drain();
    EXPECT_EQ(collector.pending(), 0u);
    auto events = collector.drain();
    EXPECT_TRUE(events.empty());
}

TEST(UsageCollector, RecordAfterDrain) {
    UsageCollector collector;
    collector.record(make_event(1));
    collector.drain();
    collector.record(make_event(2));
    auto events = collector.drain();
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].api_key_id, 2);
}

TEST(UsageCollector, DoesNotDropEventsUnderBurst) {
    UsageCollector collector;
    // Fill beyond capacity (4096)
    for (int i = 0; i < 4100; ++i) {
        collector.record(make_event(i));
    }
    EXPECT_EQ(collector.pending(), 4100u);

    auto events = collector.drain();
    ASSERT_EQ(events.size(), 4100u);
    EXPECT_EQ(events[0].api_key_id, 0);
    EXPECT_EQ(events[4099].api_key_id, 4099);
}

TEST(UsageCollector, EventFieldsPreserved) {
    UsageCollector collector;
    UsageEvent ev;
    ev.lease_token = "lease-abc";
    ev.request_id = "req-xyz";
    ev.api_key_id = 42;
    ev.user_id = 7;
    ev.account_id = 99;
    ev.group_id = 3;
    ev.model = "claude-sonnet-4-20250514";
    ev.upstream_model = "claude-sonnet-4-20250514";
    ev.input_tokens = 100;
    ev.output_tokens = 50;
    ev.cache_create_tokens = 10;
    ev.cache_read_tokens = 5;
    ev.duration_ms = 1234;
    ev.first_token_ms = 200;
    ev.stream = true;
    ev.client_disconnect = false;
    collector.record(std::move(ev));

    auto events = collector.drain();
    ASSERT_EQ(events.size(), 1u);
    auto& e = events[0];
    EXPECT_EQ(e.lease_token, "lease-abc");
    EXPECT_EQ(e.request_id, "req-xyz");
    EXPECT_EQ(e.api_key_id, 42);
    EXPECT_EQ(e.user_id, 7);
    EXPECT_EQ(e.account_id, 99);
    EXPECT_EQ(e.group_id, 3);
    EXPECT_EQ(e.model, "claude-sonnet-4-20250514");
    EXPECT_EQ(e.input_tokens, 100);
    EXPECT_EQ(e.output_tokens, 50);
    EXPECT_EQ(e.duration_ms, 1234);
    EXPECT_EQ(e.first_token_ms, 200);
    EXPECT_TRUE(e.stream);
    EXPECT_FALSE(e.client_disconnect);
}

TEST(UsageCollector, MultipleDrainCycles) {
    UsageCollector collector;
    for (int cycle = 0; cycle < 5; ++cycle) {
        for (int i = 0; i < 100; ++i) {
            collector.record(make_event(cycle * 100 + i));
        }
        auto events = collector.drain();
        ASSERT_EQ(events.size(), 100u);
        EXPECT_EQ(events[0].api_key_id, cycle * 100);
    }
    EXPECT_EQ(collector.pending(), 0u);
}

TEST(UsageCollector, DurableOutboxSurvivesReopenUntilAcknowledged) {
    auto path = "/tmp/gateway-usage-" + std::to_string(::getpid()) + ".db";
    std::filesystem::remove(path);
    {
        UsageCollector collector(path);
        auto event = make_event(77);
        event.lease_token = "lease-77";
        collector.record(std::move(event));
        EXPECT_EQ(collector.pending(), 1u);
    }
    {
        UsageCollector collector(path);
        auto events = collector.peek();
        ASSERT_EQ(events.size(), 1u);
        EXPECT_EQ(events[0].request_id, "req-77");
        collector.acknowledge("lease-77");
        EXPECT_EQ(collector.pending(), 0u);
    }
    std::filesystem::remove(path);
    std::filesystem::remove(path + "-wal");
    std::filesystem::remove(path + "-shm");
}
