#pragma once

#include <atomic>
#include <string>

class Metrics {
public:
    static Metrics& instance();

    void incrementAllowed();
    void incrementRejected();
    
    std::string serialize() const;

private:
    Metrics() = default;

    std::atomic<uint64_t> allowed_requests_{0};
    std::atomic<uint64_t> rejected_requests_{0};
};