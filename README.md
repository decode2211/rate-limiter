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

| Status | Meaning |
|---|---|
| `200 OK` | Allowed — a token was consumed. |
| `429 Too Many Requests` | Bucket empty. |
| `400 Bad Request` | Missing/non-string `client_id`, or the body isn't valid JSON. |

The examples below were actually run against a local instance with the default `config.yaml` (`capacity: 10`, `refill_rate: 2.0`).

First request for a fresh `client_id` — `200`:

```sh
$ curl -i -X POST http://localhost:8080/check -H "Content-Type: application/json" -d '{"client_id":"demo"}'
HTTP/1.1 200 OK
Content-Type: application/json

{"allowed":true,"remaining":9,"retry_after":0}
```

After 10 requests for the same `client_id` (its capacity), the 11th — `429`:

```sh
$ curl -i -X POST http://localhost:8080/check -H "Content-Type: application/json" -d '{"client_id":"demo"}'
HTTP/1.1 429 Too Many Requests
Content-Type: application/json

{"allowed":false,"remaining":0,"retry_after":1}
```

A body that isn't valid JSON — `400`:

```sh
$ curl -i -X POST http://localhost:8080/check -H "Content-Type: application/json" -d 'not-json'
HTTP/1.1 400 Bad Request
Content-Type: application/json

{"error":"Invalid JSON payload"}
```

(A well-formed JSON body missing `client_id`, or with a non-string `client_id`, returns the same `400` with `{"error":"Missing or invalid 'client_id' field"}` instead.)

### `GET /metrics`

Prometheus text exposition format. `Content-Type: text/plain; version=0.0.4`. No auth, no parameters.

Real output captured from a local instance after a mix of allowed, rate-limited, and malformed traffic:

```
# HELP rate_limiter_requests_received_total Total number of POST /check requests received.
# TYPE rate_limiter_requests_received_total counter
rate_limiter_requests_received_total 33

# HELP rate_limiter_requests_total Total number of processed rate limit checks, by outcome.
# TYPE rate_limiter_requests_total counter
rate_limiter_requests_total{status="allowed"} 22
rate_limiter_requests_total{status="rejected"} 10

# HELP rate_limiter_requests_aggregate_total Total cumulative checks (allowed + rejected).
# TYPE rate_limiter_requests_aggregate_total counter
rate_limiter_requests_aggregate_total 32

# HELP rate_limiter_active_buckets Number of client buckets currently tracked in memory.
# TYPE rate_limiter_active_buckets gauge
rate_limiter_active_buckets 2

# HELP rate_limiter_buckets_evicted_total Total number of idle client buckets evicted.
# TYPE rate_limiter_buckets_evicted_total counter
rate_limiter_buckets_evicted_total 0

# HELP rate_limiter_check_duration_seconds Latency of POST /check requests.
# TYPE rate_limiter_check_duration_seconds histogram
rate_limiter_check_duration_seconds_bucket{le="0.0001"} 31
rate_limiter_check_duration_seconds_bucket{le="0.00025"} 31
rate_limiter_check_duration_seconds_bucket{le="0.0005"} 31
rate_limiter_check_duration_seconds_bucket{le="0.001"} 31
rate_limiter_check_duration_seconds_bucket{le="0.0025"} 32
rate_limiter_check_duration_seconds_bucket{le="0.005"} 32
rate_limiter_check_duration_seconds_bucket{le="0.01"} 32
rate_limiter_check_duration_seconds_bucket{le="0.05"} 32
rate_limiter_check_duration_seconds_bucket{le="+Inf"} 33
rate_limiter_check_duration_seconds_sum 0.118448
rate_limiter_check_duration_seconds_count 33
```

| Metric | Type | Meaning |
|---|---|---|
| `rate_limiter_requests_received_total` | counter | Every `POST /check` received, including malformed (`400`) ones. |
| `rate_limiter_requests_total{status="allowed"\|"rejected"}` | counter | Only requests that reached the rate limiter (excludes `400`s). |
| `rate_limiter_requests_aggregate_total` | counter | `allowed + rejected`. |
| `rate_limiter_active_buckets` | gauge | Client buckets currently held in memory right now. |
| `rate_limiter_buckets_evicted_total` | counter | Idle buckets removed by the eviction sweep, cumulative. |
| `rate_limiter_check_duration_seconds` | histogram | End-to-end `/check` handler latency, recorded on every request including malformed ones. |

`requests_received_total` (33) is one more than `allowed + rejected` (22 + 10 = 32) in the capture above: one of the 33 requests was the malformed-JSON example, which is received and timed but never reaches the rate limiter, so it isn't counted as allowed or rejected.

### Graceful shutdown

The process installs `SIGINT`/`SIGTERM` handlers (see [Building and running locally](#building-and-running-locally)): on either signal it stops accepting new connections, lets in-flight requests finish, then exits `0`. It does not currently expose this as an HTTP-level "draining" state — a request that arrives after the signal but before the listener actually stops is served normally.

### No `/health` endpoint

There is no dedicated liveness/readiness route. `GET /metrics` responding with `200` is the closest available signal today for an orchestrator health check.

## Known limitations

- **Single-instance only** — see [Configuration](#configuration). There is no shared/distributed backend, so multiple replicas do not share rate-limit state.
- **No `/health` endpoint** — see [API](#api) above.

See [PROJECT_AUDIT.md](PROJECT_AUDIT.md) for the full engineering audit, including the Redis-backend analysis, a module-by-module status breakdown, and what genuinely remains before a production release.
