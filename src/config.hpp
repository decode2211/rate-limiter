#pragma once

#include <string>

struct ServerConfig {
    std::string host = "0.0.0.0";
    int port = 8080;
};

struct RateLimitConfig {
    double capacity = 10.0;
    double refill_rate = 2.0;
    // See EvictionConfig in rate_limiter.hpp for what these control.
    double idle_ttl_multiplier = 10.0;
    double sweep_interval_seconds = 60.0;
};

class Config {
public:
    ServerConfig server;
    RateLimitConfig rate_limit;

    // Throws std::runtime_error if the file is missing, is not valid YAML,
    // or contains a semantically invalid value (e.g. non-positive capacity
    // or refill_rate, an out-of-range port). Fields simply omitted from an
    // otherwise-valid file keep their struct defaults above.
    static Config loadFromFile(const std::string& filepath);
};