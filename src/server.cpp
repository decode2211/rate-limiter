#include "server.hpp"
#include <nlohmann/json.hpp>
#include <iostream>

using json = nlohmann::json;

Server::Server(const Config& config)
    : config_(config),
      rate_limiter_(config.rate_limit.capacity, config.rate_limit.refill_rate,
                    EvictionConfig{config.rate_limit.idle_ttl_multiplier, config.rate_limit.sweep_interval_seconds}) {
    setupRoutes();
}

void Server::setupRoutes() {
    http_server_.Post("/check", [this](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Content-Type", "application/json");

        try {
            auto body = json::parse(req.body);
            if (!body.contains("client_id") || !body["client_id"].is_string()) {
                res.status = 400;
                res.set_content(json{{"error", "Missing or invalid 'client_id' field"}}.dump(), "application/json");
                return;
            }

            std::string client_id = body["client_id"].get<std::string>();
            RateLimitResult result = rate_limiter_.check(client_id);

            json response = {
                {"allowed", result.allowed},
                {"remaining", static_cast<uint64_t>(result.remaining_tokens)},
                {"retry_after", result.retry_after_seconds}
            };

            res.status = result.allowed ? 200 : 429;
            res.set_content(response.dump(), "application/json");

        } catch (const json::parse_error&) {
            res.status = 400;
            res.set_content(json{{"error", "Invalid JSON payload"}}.dump(), "application/json");
        }
    });
}

void Server::start() {
    std::cout << "Rate Limiter Service listening on http://" 
              << config_.server.host << ":" << config_.server.port << std::endl;
    http_server_.listen(config_.server.host.c_str(), config_.server.port);
}

void Server::stop() {
    http_server_.stop();
}