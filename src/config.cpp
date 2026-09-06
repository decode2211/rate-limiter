#include "config.hpp"
#include <yaml-cpp/yaml.h>
#include <stdexcept>

namespace {

// Values that are merely omitted from config.yaml keep their struct
// defaults (see config.hpp) — that's a deliberate, documented convenience.
// Values that are *present but nonsensical* are rejected here instead of
// being allowed to misbehave later at request time (e.g. a zero
// refill_rate previously caused a division-by-zero deep in TokenBucket).
void validate(const Config& config) {
    if (config.rate_limit.capacity <= 0.0) {
        throw std::runtime_error(
            "rate_limit.capacity must be > 0 (got " + std::to_string(config.rate_limit.capacity) + ")");
    }
    if (config.rate_limit.refill_rate <= 0.0) {
        throw std::runtime_error(
            "rate_limit.refill_rate must be > 0 (got " + std::to_string(config.rate_limit.refill_rate) + ")");
    }
    if (config.rate_limit.idle_ttl_multiplier <= 0.0) {
        throw std::runtime_error(
            "rate_limit.idle_ttl_multiplier must be > 0 (got " + std::to_string(config.rate_limit.idle_ttl_multiplier) + ")");
    }
    if (config.rate_limit.sweep_interval_seconds <= 0.0) {
        throw std::runtime_error(
            "rate_limit.sweep_interval_seconds must be > 0 (got " + std::to_string(config.rate_limit.sweep_interval_seconds) + ")");
    }
    if (config.server.port <= 0 || config.server.port > 65535) {
        throw std::runtime_error(
            "server.port must be between 1 and 65535 (got " + std::to_string(config.server.port) + ")");
    }
    if (config.server.host.empty()) {
        throw std::runtime_error("server.host must not be empty");
    }
    if (config.redis.enabled) {
        if (config.redis.host.empty()) {
            throw std::runtime_error("redis.host must not be empty when redis.enabled is true");
        }
        if (config.redis.port <= 0 || config.redis.port > 65535) {
            throw std::runtime_error(
                "redis.port must be between 1 and 65535 (got " + std::to_string(config.redis.port) + ")");
        }
    }
}

} // namespace

Config Config::loadFromFile(const std::string& filepath) {
    YAML::Node root;
    try {
        root = YAML::LoadFile(filepath);
    } catch (const std::exception& e) {
        throw std::runtime_error("could not read config file '" + filepath + "': " + e.what());
    }

    Config config;
    try {
        if (root["server"]) {
            if (root["server"]["host"]) config.server.host = root["server"]["host"].as<std::string>();
            if (root["server"]["port"]) config.server.port = root["server"]["port"].as<int>();
        }

        if (root["rate_limit"]) {
            if (root["rate_limit"]["capacity"]) config.rate_limit.capacity = root["rate_limit"]["capacity"].as<double>();
            if (root["rate_limit"]["refill_rate"]) config.rate_limit.refill_rate = root["rate_limit"]["refill_rate"].as<double>();
            if (root["rate_limit"]["idle_ttl_multiplier"]) config.rate_limit.idle_ttl_multiplier = root["rate_limit"]["idle_ttl_multiplier"].as<double>();
            if (root["rate_limit"]["sweep_interval_seconds"]) config.rate_limit.sweep_interval_seconds = root["rate_limit"]["sweep_interval_seconds"].as<double>();
        }

        if (root["redis"]) {
            if (root["redis"]["enabled"]) config.redis.enabled = root["redis"]["enabled"].as<bool>();
            if (root["redis"]["host"]) config.redis.host = root["redis"]["host"].as<std::string>();
            if (root["redis"]["port"]) config.redis.port = root["redis"]["port"].as<int>();
        }
    } catch (const std::exception& e) {
        throw std::runtime_error("invalid value in config file '" + filepath + "': " + e.what());
    }

    validate(config);
    return config;
}