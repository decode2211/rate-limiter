# Token Bucket Rate Limiter

A high-performance, thread-safe rate limiting service built in C++ using the Token Bucket algorithm.

## Overview

This project implements a rate limiter that controls how many requests a client can make within a given period.

Each client gets a token bucket:

- Requests consume tokens.
- Tokens are refilled at a fixed rate.
- Requests are allowed when tokens are available.
- Requests are rejected with HTTP `429` when the bucket is empty.

Buckets are created lazily per `client_id`, held in memory, and evicted automatically once idle for long enough (see [Configuration](#configuration)) — there is no persistence and no shared state across multiple instances of the service; each instance tracks its own clients independently.

## Example

Configuration:

```text
Bucket Capacity : 10 tokens
Refill Rate     : 2 tokens/second
```

With this configuration, a client can make 10 requests immediately, then up to 2 more requests per second as tokens refill.

## Architecture

```
main.cpp        entry point: loads config, installs SIGINT/SIGTERM handlers, starts the HTTP server
config          loads server / rate_limit settings from config.yaml, validates them at startup
token_bucket    core algorithm — single-client token bucket (thread-safe)
rate_limiter    per-client manager — maps client_id -> TokenBucket, evicts idle buckets
server          HTTP layer (cpp-httplib) — exposes POST /check and GET /metrics
metrices        Prometheus-style Metrics singleton, wired into both routes above
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

If no config path is given, the binary looks for `config.yaml` in the current working directory. If the config file is missing, isn't valid YAML, or contains an invalid value, the process prints a `Fatal:` message to stderr and exits with a non-zero status instead of starting (see [Configuration](#configuration) for exactly which values are validated).

The service shuts down gracefully on `SIGINT`/`SIGTERM`: it stops accepting new connections, lets in-flight requests finish, then exits 0. You'll see:

```
Shutdown requested, stopping server...
Server stopped cleanly.
```

## Running with Docker

```sh
docker compose up --build
```

This builds the multi-stage image and starts the service on `http://localhost:8080`, bind-mounting `config.yaml` into the container so config changes take effect on restart without a rebuild. `docker compose stop` (or `down`) sends `SIGTERM` to the process directly (it's the container's PID 1 per the Dockerfile's `ENTRYPOINT`), triggering the same graceful shutdown described above.

## Testing

```sh
cd build
ctest --output-on-failure
```

17 tests (GoogleTest, in `tests/rate_limiter_test.cpp`) cover: the token bucket algorithm including zero/negative refill-rate edge cases, per-client isolation and thread safety, idle-bucket eviction, config validation (rejecting non-positive capacity/refill-rate, an out-of-range port, malformed YAML, a missing file), and the Metrics counters/gauge. HTTP-layer behavior (status codes at the routing layer) is not currently covered by automated tests — it's exercised manually (see [API](#api) below).

## Benchmarking

```sh
./build/benchmark
```

Runs a standalone, in-process multi-threaded load test (8 threads x 100,000 requests against 4 simulated clients) and prints throughput and p50/p95/p99 latency. It exercises `RateLimiter` directly, not the HTTP layer.

## Configuration

All configuration is read from `config.yaml` (or the path passed as the first CLI argument). There are no environment variables — the service does not read any `getenv` values.

```yaml
server:
  host: "0.0.0.0"
  port: 8080

rate_limit:
  capacity: 10
  refill_rate: 2.0
  idle_ttl_multiplier: 10.0
  sweep_interval_seconds: 60.0
```

| Key | Type | Default | Purpose |
|---|---|---|---|
| `server.host` | string | `"0.0.0.0"` | Interface the HTTP server binds to. |
| `server.port` | integer | `8080` | Port the HTTP server listens on. |
| `rate_limit.capacity` | float | `10` | Maximum tokens a client's bucket can hold — its allowed burst size. |
| `rate_limit.refill_rate` | float | `2.0` | Tokens added per second. |
| `rate_limit.idle_ttl_multiplier` | float | `10.0` | A client's bucket is evicted once it has sat idle for `idle_ttl_multiplier * (capacity / refill_rate)` seconds — long enough that it would have fully refilled several times over, so evicting it is behaviorally identical to that `client_id` never having been seen. See `src/rate_limiter.hpp`'s `EvictionConfig` for the full reasoning. |
| `rate_limit.sweep_interval_seconds` | float | `60.0` | Minimum time between eviction sweeps, so a flood of unique `client_id`s doesn't pay for a full sweep on every single new one. |

**Omitted vs. invalid keys are handled differently, on purpose.** A key simply left out of `config.yaml` falls back to its default above — that's a deliberate, supported convenience (a file specifying only `rate_limit.capacity`, say, is valid). A key that's *present but semantically invalid* is a hard startup failure, not a silent fallback: the process prints a `Fatal: <reason>` message to stderr and exits non-zero rather than running with unintended limits. At load time, `Config::loadFromFile` rejects:

- a missing config file, or one that isn't valid YAML
- `rate_limit.capacity <= 0`
- `rate_limit.refill_rate <= 0`
- `rate_limit.idle_ttl_multiplier <= 0`
- `rate_limit.sweep_interval_seconds <= 0`
- `server.port` outside `1`–`65535`
- an empty `server.host`

There is no distributed/shared-backend configuration (e.g. Redis) — the service is in-memory and single-instance only. Running multiple replicas behind a load balancer gives each replica an independent view of every client's quota, which is not shared rate limiting; there's no built-in way around that today. See `PROJECT_AUDIT.md` for what adding a shared backend would actually involve.

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

## Known limitations

- **Single-instance only** — state is in-process memory; running multiple replicas gives each an independent view of every client's quota. The `redis` config block is a placeholder for a future shared backend and has no effect today.
- **No bucket eviction** — a bucket is created for every distinct `client_id` seen and is never removed, so memory grows with the number of unique clients over the process lifetime.
- **No `/health` or `/metrics` endpoint** — a `Metrics` counter class exists in `src/metrices.cpp` but is not yet wired into the server.
- **No graceful shutdown** — the process has no signal handling; stopping it does not drain in-flight requests.

See [PROJECT_AUDIT.md](PROJECT_AUDIT.md) for a full engineering audit of the current state of the codebase, including a module-by-module status breakdown and a prioritized list of what's left before a production release.
