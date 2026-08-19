#pragma once

#include "config.hpp"
#include "rate_limiter.hpp"
#include <httplib.h>
#include <memory>

class Server {
public:
    Server(const Config& config);

    void start();
    void stop();

private:
    void setupRoutes();

    Config config_;
    RateLimiter rate_limiter_;
    httplib::Server http_server_;
};