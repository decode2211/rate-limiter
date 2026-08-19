#include <gtest/gtest.h>
#include "token_bucket.hpp"
#include "rate_limiter.hpp"
#include <thread>
#include <vector>

TEST(TokenBucketTest, AllowsRequestsUpToCapacity) {
    TokenBucket bucket(3.0, 1.0);

    EXPECT_TRUE(bucket.consume().allowed);
    EXPECT_TRUE(bucket.consume().allowed);
    EXPECT_TRUE(bucket.consume().allowed);
    EXPECT_FALSE(bucket.consume().allowed);
}

TEST(TokenBucketTest, RefillsTokensOverTime) {
    TokenBucket bucket(2.0, 2.0); // 2 tokens/sec

    EXPECT_TRUE(bucket.consume().allowed);
    EXPECT_TRUE(bucket.consume().allowed);
    EXPECT_FALSE(bucket.consume().allowed);

    std::this_thread::sleep_for(std::chrono::milliseconds(1050)); // Wait ~1 sec

    EXPECT_TRUE(bucket.consume().allowed);
    EXPECT_TRUE(bucket.consume().allowed);
    EXPECT_FALSE(bucket.consume().allowed);
}

TEST(RateLimiterTest, HandlesMultipleClientsIndependently) {
    RateLimiter limiter(1.0, 1.0);

    EXPECT_TRUE(limiter.check("client_A").allowed);
    EXPECT_FALSE(limiter.check("client_A").allowed);

    EXPECT_TRUE(limiter.check("client_B").allowed);
    EXPECT_FALSE(limiter.check("client_B").allowed);
}

TEST(RateLimiterTest, ThreadSafetyUnderConcurrency) {
    RateLimiter limiter(100.0, 10.0);
    constexpr int threads = 10;
    constexpr int requests_per_thread = 10;

    std::vector<std::thread> workers;
    for (int i = 0; i < threads; ++i) {
        workers.emplace_back([&limiter]() {
            for (int j = 0; j < requests_per_thread; ++j) {
                limiter.check("concurrent_client");
            }
        });
    }

    for (auto& t : workers) {
        t.join();
    }

    RateLimitResult result = limiter.check("concurrent_client");
    EXPECT_FALSE(result.allowed); // 100 capacity exhausted by 100 calls
}