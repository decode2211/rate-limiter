#include "server.hpp"
#include "metrices.hpp"
#include <nlohmann/json.hpp>
#include <chrono>
#include <iostream>

using json = nlohmann::json;

namespace {

// RAII so latency is recorded on every exit path of the /check handler
// below (the two early "return"s on invalid input, the json::parse_error
// catch, and normal completion) without duplicating the measurement code
// at each one.
class LatencyRecorder {
public:
    LatencyRecorder() : start_(std::chrono::steady_clock::now()) {}
    ~LatencyRecorder() {
        std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start_;
        Metrics::instance().recordRequestLatency(elapsed.count());
    }

private:
    std::chrono::steady_clock::time_point start_;
};

} // namespace

Server::Server(const Config& config)
    : config_(config),
      rate_limiter_(config.rate_limit.capacity, config.rate_limit.refill_rate,
                    EvictionConfig{config.rate_limit.idle_ttl_multiplier, config.rate_limit.sweep_interval_seconds}) {
    setupRoutes();
}

void Server::setupRoutes() {
    http_server_.Post("/check", [this](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Content-Type", "application/json");
        Metrics::instance().incrementRequestsReceived();
        LatencyRecorder latency_recorder; // records on every return path below, via its destructor

        try {
            auto body = json::parse(req.body);
            if (!body.contains("client_id") || !body["client_id"].is_string()) {
                res.status = 400;
                res.set_content(json{{"error", "Missing or invalid 'client_id' field"}}.dump(), "application/json");
                return;
            }

            std::string client_id = body["client_id"].get<std::string>();
            RateLimitResult result = rate_limiter_.check(client_id);

            if (result.allowed) {
                Metrics::instance().incrementAllowed();
            } else {
                Metrics::instance().incrementRejected();
            }

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

    http_server_.Get("/metrics", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(Metrics::instance().serialize(), "text/plain; version=0.0.4");
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