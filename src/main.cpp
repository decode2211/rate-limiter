#include "config.hpp"
#include "server.hpp"
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <thread>

namespace {

// The only things the C++ standard guarantees are safe inside a signal
// handler are lock-free atomic ops and assignment to a volatile
// sig_atomic_t -- calling Server::stop() (which touches non-trivial
// cpp-httplib state) directly from the handler would not be
// async-signal-safe. So the handler only sets this flag; a watcher
// thread below polls it and does the actual stop() call from ordinary
// (non-signal) context.
volatile std::sig_atomic_t g_shutdown_requested = 0;

void handleShutdownSignal(int /*signum*/) {
    g_shutdown_requested = 1;
}

} // namespace

int main(int argc, char* argv[]) {
    std::string config_path = "config.yaml";
    if (argc > 1) {
        config_path = argv[1];
    }

    Config config;
    try {
        config = Config::loadFromFile(config_path);
    } catch (const std::exception& e) {
        std::cerr << "Fatal: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    std::signal(SIGINT, handleShutdownSignal);
    std::signal(SIGTERM, handleShutdownSignal);

    Server server(config);

    std::thread watcher([&server]() {
        while (!g_shutdown_requested) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::cerr << "Shutdown requested, stopping server..." << std::endl;
        server.stop();
    });

    server.start(); // blocks until server.stop() is called (by the watcher,
                     // above) or the listener fails to start

    // Wake the watcher even if start() returned for a reason other than a
    // signal (e.g. a failed bind), so it can't block main() forever on
    // join() waiting for a signal that will never arrive. Calling stop()
    // again here if the watcher already called it is a harmless no-op.
    g_shutdown_requested = 1;
    watcher.join();

    std::cerr << "Server stopped cleanly." << std::endl;
    return EXIT_SUCCESS;
}
