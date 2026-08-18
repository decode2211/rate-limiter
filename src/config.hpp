#pragma once

#include <string>

struct ServerConfig {
    std::string host = "0.0.0.0";
    int port = 8080;
};

struct RateLimitConfig {
    double capacity = 10.0;
    double refill_rate = 2.0;
};

struct RedisConfig {
    bool enabled = false;
    std::string host = "127.0.0.1";
    int port = 6379;
};

class Config {
public:
    ServerConfig server;
    RateLimitConfig rate_limit;
    RedisConfig redis;

    static Config loadFromFile(const std::string& filepath);
};