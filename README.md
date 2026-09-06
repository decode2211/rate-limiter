# Token Bucket Rate Limiter

A high-performance, thread-safe rate limiting service built in C++ using the Token Bucket algorithm.

## Overview

This project implements a rate limiter that controls how many requests a client can make within a given period.

Each client gets a token bucket:

- Requests consume tokens.
- Tokens are refilled at a fixed rate.
- Requests are allowed when tokens are available.
- Requests are rejected with HTTP `429` when the bucket is empty.

Buckets are created lazily per `client_id` and held in memory for the lifetime of the process — there is no persistence and no shared state across multiple instances of the service.

## Example

Configuration:

```text
Bucket Capacity : 10 tokens
Refill Rate     : 2 tokens/second
```

With this configuration, a client can make 10 requests immediately, then up to 2 more requests per second as tokens refill.

## Architecture

```
main.cpp        entry point: loads config, starts the HTTP server
config          loads server / rate_limit / redis settings from config.yaml
token_bucket    core algorithm — single-client token bucket (thread-safe)
rate_limiter    per-client manager — maps client_id -> TokenBucket
server          HTTP layer (cpp-httplib) — exposes POST /check
metrices        Prometheus-style counters (allowed/rejected requests)
```

Source lives in `src/`, unit tests in `tests/`, a standalone throughput/latency benchmark in `benchmarks/`.

## Requirements

- CMake >= 3.16
- A C++17 compiler (GCC, Clang, or MSVC)
- Network access when configuring for the first time — dependencies (`nlohmann/json`, `cpp-httplib`, `yaml-cpp`, `googletest`) are fetched automatically via CMake `FetchContent`; there is no vendoring or offline build path.

## Building and running locally

```sh
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build . --config Release -j$(nproc)

# from the build directory:
./rate_limiter ../config.yaml
```

If no config path is given, the binary looks for `config.yaml` in the current working directory.

## Running with Docker

```sh
docker compose up --build
```

This builds the multi-stage image, starts the service on `http://localhost:8080`, and bind-mounts `config.yaml` into the container so config changes take effect on restart without a rebuild.

## Configuration

All configuration is read from `config.yaml` (or the path passed as the first CLI argument). There are no required environment variables — the service does not read any `getenv` values.

```yaml
server:
  host: "0.0.0.0"
  port: 8080

rate_limit:
  capacity: 10          # Maximum number of tokens a bucket can hold
  refill_rate: 2.0       # Tokens added per second

redis:
  enabled: false
  host: "127.0.0.1"
  port: 6379
```

- `server` — host/port the HTTP server binds to.
- `rate_limit` — global default capacity and refill rate applied to every client's bucket. Set `capacity`/`refill_rate` to a value greater than 0; a zero or negative `refill_rate` is not validated and will produce incorrect `retry_after` values.
- `redis` — reserved for a future distributed/shared-state backend. **Currently unused**: the field is parsed but no Redis client is implemented, and the service only ever runs with independent, unshared in-memory state per instance.

Any section or field left out of `config.yaml` falls back to the defaults shown above. If the file is missing or invalid, the service logs a warning to stderr and starts with default settings rather than failing to start.

## API

### `POST /check`

Request body:

```json
{ "client_id": "some-client" }
```

Response (`200 OK` — allowed):

```json
{ "allowed": true, "remaining": 9, "retry_after": 0 }
```

Response (`429 Too Many Requests` — rejected):

```json
{ "allowed": false, "remaining": 0, "retry_after": 3 }
```

Response (`400 Bad Request` — missing/invalid `client_id`, or invalid JSON):

```json
{ "error": "Missing or invalid 'client_id' field" }
```

Example:

```sh
curl -X POST http://localhost:8080/check \
  -H "Content-Type: application/json" \
  -d '{"client_id": "test-client"}'
```

## Testing

```sh
cd build
ctest --output-on-failure
```

Unit tests (GoogleTest, in `tests/rate_limiter_test.cpp`) cover the token bucket algorithm and per-client isolation/concurrency in `RateLimiter`. HTTP-layer behavior (status codes, error handling) and config parsing are not currently covered by automated tests.

## Benchmarking

```sh
./build/benchmark
```

Runs a standalone, in-process multi-threaded load test (8 threads x 100,000 requests against 4 simulated clients) and prints throughput and p50/p95/p99 latency. It exercises `RateLimiter` directly, not the HTTP layer.

## Known limitations

- **Single-instance only** — state is in-process memory; running multiple replicas gives each an independent view of every client's quota. The `redis` config block is a placeholder for a future shared backend and has no effect today.
- **No bucket eviction** — a bucket is created for every distinct `client_id` seen and is never removed, so memory grows with the number of unique clients over the process lifetime.
- **No `/health` or `/metrics` endpoint** — a `Metrics` counter class exists in `src/metrices.cpp` but is not yet wired into the server.
- **No graceful shutdown** — the process has no signal handling; stopping it does not drain in-flight requests.

See [PROJECT_AUDIT.md](PROJECT_AUDIT.md) for a full engineering audit of the current state of the codebase, including a module-by-module status breakdown and a prioritized list of what's left before a production release.
