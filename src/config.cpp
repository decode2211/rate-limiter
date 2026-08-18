#include "config.hpp"
#include <yaml-cpp/yaml.h>
#include <iostream>

Config Config::loadFromFile(const std::string& filepath) {
    Config config;
    try {
        YAML::Node root = YAML::LoadFile(filepath);

        if (root["server"]) {
            if (root["server"]["host"]) config.server.host = root["server"]["host"].as<std::string>();
            if (root["server"]["port"]) config.server.port = root["server"]["port"].as<int>();
        }

        if (root["rate_limit"]) {
            if (root["rate_limit"]["capacity"]) config.rate_limit.capacity = root["rate_limit"]["capacity"].as<double>();
            if (root["rate_limit"]["refill_rate"]) config.rate_limit.refill_rate = root["rate_limit"]["refill_rate"].as<double>();
        }

        if (root["redis"]) {
            if (root["redis"]["enabled"]) config.redis.enabled = root["redis"]["enabled"].as<bool>();
            if (root["redis"]["host"]) config.redis.host = root["redis"]["host"].as<std::string>();
            if (root["redis"]["port"]) config.redis.port = root["redis"]["port"].as<int>();
        }
    } catch (const std::exception& e) {
        std::cerr << "Warning: Could not load " << filepath << " (" << e.what() << "). Using default settings.\n";
    }
    return config;
}