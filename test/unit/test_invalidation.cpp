#include <gtest/gtest.h>
#include "auth/invalidation_subscriber.h"

using gateway::auth::InvalidationVersionTracker;

TEST(InvalidationVersionTracker, InitialVersionEstablishesBaseline) {
    InvalidationVersionTracker tracker;

    EXPECT_FALSE(tracker.observe(true, "1"));
    EXPECT_FALSE(tracker.observe(true, "1"));
}

TEST(InvalidationVersionTracker, VersionChangeFlushesOnce) {
    InvalidationVersionTracker tracker;

    EXPECT_FALSE(tracker.observe(true, "1"));
    EXPECT_TRUE(tracker.observe(true, "2"));
    EXPECT_FALSE(tracker.observe(true, "2"));
}

TEST(InvalidationVersionTracker, MissingVersionFlushesAndRecoveryReestablishesBaseline) {
    InvalidationVersionTracker tracker;

    EXPECT_FALSE(tracker.observe(true, "7"));
    EXPECT_TRUE(tracker.observe(false, ""));
    EXPECT_FALSE(tracker.observe(false, ""));
    EXPECT_FALSE(tracker.observe(true, "8"));
    EXPECT_TRUE(tracker.observe(true, "9"));
}
