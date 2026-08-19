#include "rate_limiter.hpp"
#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <algorithm>
#include <numeric>
#include <iomanip>

int main() {
    constexpr int NUM_THREADS = 8;
    constexpr int REQUESTS_PER_THREAD = 100000;
    constexpr int TOTAL_REQUESTS = NUM_THREADS * REQUESTS_PER_THREAD;

    // Initialize RateLimiter with 500,000 capacity and 100,000 refill rate
    RateLimiter limiter(500000.0, 100000.0);

    std::vector<std::thread> threads;
    std::vector<std::vector<double>> thread_latencies(NUM_THREADS);

    std::cout << "Starting benchmark with " << NUM_THREADS 
              << " threads processing " << TOTAL_REQUESTS << " total requests...\n";

    auto start_time = std::chrono::high_resolution_clock::now();

    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([t, &limiter, &thread_latencies]() {
            thread_latencies[t].reserve(REQUESTS_PER_THREAD);
            std::string client_id = "client_" + std::to_string(t % 4); // Simulate 4 distinct clients

            for (int i = 0; i < REQUESTS_PER_THREAD; ++i) {
                auto req_start = std::chrono::high_resolution_clock::now();
                
                limiter.check(client_id);
                
                auto req_end = std::chrono::high_resolution_clock::now();
                std::chrono::duration<double, std::nano> elapsed = req_end - req_start;
                thread_latencies[t].push_back(elapsed.count());
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> total_duration = end_time - start_time;

    // Flatten latencies into single vector for percentile calculation
    std::vector<double> all_latencies;
    all_latencies.reserve(TOTAL_REQUESTS);
    for (const auto& latencies : thread_latencies) {
        all_latencies.insert(all_latencies.end(), latencies.begin(), latencies.end());
    }

    std::sort(all_latencies.begin(), all_latencies.end());

    double sum = std::accumulate(all_latencies.begin(), all_latencies.end(), 0.0);
    double avg_latency_ns = sum / TOTAL_REQUESTS;
    double p50_latency_ns = all_latencies[static_cast<size_t>(TOTAL_REQUESTS * 0.50)];
    double p95_latency_ns = all_latencies[static_cast<size_t>(TOTAL_REQUESTS * 0.95)];
    double p99_latency_ns = all_latencies[static_cast<size_t>(TOTAL_REQUESTS * 0.99)];
    double rps = TOTAL_REQUESTS / total_duration.count();

    std::cout << "\n================ Benchmark Results ================\n";
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "Total Requests Processed : " << TOTAL_REQUESTS << "\n";
    std::cout << "Total Time Elapsed       : " << total_duration.count() << " seconds\n";
    std::cout << "Throughput               : " << rps << " req/sec\n";
    std::cout << "Average Latency          : " << avg_latency_ns / 1000.0 << " us\n";
    std::cout << "P50 Latency              : " << p50_latency_ns / 1000.0 << " us\n";
    std::cout << "P95 Latency              : " << p95_latency_ns / 1000.0 << " us\n";
    std::cout << "P99 Latency              : " << p99_latency_ns / 1000.0 << " us\n";
    std::cout << "===================================================\n";

    return 0;
}