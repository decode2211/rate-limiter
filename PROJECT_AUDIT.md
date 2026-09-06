# Project Audit — Token Bucket Rate Limiter

Audit date: 2026-09-06. Scope: full repository, read-only review against actual source (not README claims).

---

## PHASE 1 — MAP

### Directory structure

```
.
├── CMakeLists.txt          # Build system definition (CMake)
├── Dockerfile              # Multi-stage build → runtime image
├── docker-compose.yml      # Single-service compose file
├── config.yaml             # Runtime config (server/rate_limit/redis)
├── README.md               # Incomplete — cuts off mid code-block (see below)
├── src/                    # All application source (flat, no subfolders)
│   ├── main.cpp            # Process entry point
│   ├── config.hpp/.cpp     # Config structs + YAML loader
│   ├── token_bucket.hpp/.cpp   # Core algorithm: single-client token bucket
│   ├── rate_limiter.hpp/.cpp   # Multi-client wrapper over TokenBucket
│   ├── server.hpp/.cpp     # HTTP layer (cpp-httplib wrapper, routes)
│   └── metrices.hpp/.cpp   # Prometheus-style metrics singleton [sic — misspelled "metrices"]
├── tests/
│   └── rate_limiter_test.cpp   # GoogleTest unit tests
└── benchmarks/
    └── benchmark.cpp       # Standalone multi-threaded throughput/latency benchmark
```

There is no `.github/`, no `.gitignore`, no `.env*` file, and no `docs/` folder. `git ls-files` returns 18 tracked files total — this is a small, single-purpose repo.

### Language / framework / build / package management

- **Language:** C++17 (`CMakeLists.txt:4-6`, `set(CMAKE_CXX_STANDARD 17)`).
- **Build system:** CMake ≥ 3.16 (`CMakeLists.txt:1`), using `FetchContent` to pull dependencies at configure time — there is no vendoring and no package manager (no Conan/vCPPkg lockfile). Dependencies and pinned versions (`CMakeLists.txt:19-49`):
  - `nlohmann/json` v3.11.3 (JSON)
  - `yhirose/cpp-httplib` v0.15.3 (HTTP server)
  - `jbeder/yaml-cpp` v0.8.0 (YAML config parsing)
  - `google/googletest` v1.14.0 (test framework)
  - A workaround comment (`CMakeLists.txt:8-13`) notes yaml-cpp 0.8.0's own `cmake_minimum_required` is too old for CMake 4.x and forces `CMAKE_POLICY_VERSION_MINIMUM 3.5` — confirms this was built/tested against a modern CMake toolchain.
- **No package.json / pyproject / requirements.txt** — this is a pure C++ project, not JS/Python.
- **Containerization:** Docker multi-stage build (`Dockerfile`), `ubuntu:22.04` for both builder and runtime stages.

### Entry points

- **Process entry point:** [src/main.cpp](src/main.cpp) — loads config from `argv[1]` or defaults to `"config.yaml"` (relative path), constructs `Server`, calls `server.start()` (blocking).
- **HTTP route(s):** exactly one route is registered — `POST /check` in [src/server.cpp:14-42](src/server.cpp#L14-L42).
- **Test binary:** `rate_limiter_test` built from [tests/rate_limiter_test.cpp](tests/rate_limiter_test.cpp), registered via `gtest_discover_tests` (`CMakeLists.txt:80`).
- **Benchmark binary:** `benchmark` built from [benchmarks/benchmark.cpp](benchmarks/benchmark.cpp), a standalone `main()`, not wired into CTest/CI.

### Config files read

- [config.yaml](config.yaml): three sections — `server` (host/port), `rate_limit` (capacity/refill_rate), `redis` (enabled/host/port, currently `enabled: false`).
- [Dockerfile](Dockerfile): builds in an `ubuntu:22.04` builder stage, copies only the built binary + `config.yaml` into a slim `ubuntu:22.04` runner stage, `EXPOSE 8080`, `ENTRYPOINT ["/app/rate_limiter"]`.
- [docker-compose.yml](docker-compose.yml): single service, binds host port 8080, bind-mounts `./config.yaml` over the container's copy, `restart: unless-stopped`.
- No CI workflow files exist anywhere in the repo (no `.github/workflows/*`) — UNVERIFIED whether tests are run anywhere automatically; based on file search, they are not.
- No `.env` / `.env.example` — the app takes zero environment variables (confirmed by grepping source for `getenv`/`std::getenv` — no matches). All configuration is via `config.yaml` or the CLI arg.

---

## PHASE 2 — WHAT IT DOES

### Purpose

**Plain language:** A small standalone web service that answers "is this client allowed to make a request right now?" Each named client gets its own budget of tokens that refills over time; when a client asks and has tokens left, it's allowed and a token is spent; when it's out, the request is rejected until enough time passes to refill.

**Technical:** A single-endpoint HTTP microservice implementing the token-bucket rate-limiting algorithm in-memory, per-client, with thread-safe concurrent access, intended to run as a sidecar/shared service that other systems call before processing a client's actual request.

### Core flow: `POST /check`

1. **Request arrives** at cpp-httplib's registered handler for `POST /check` — [src/server.cpp:14](src/server.cpp#L14).
2. **Parse & validate:** body parsed as JSON (`json::parse`, [server.cpp:18](src/server.cpp#L18)); must contain a string field `client_id` ([server.cpp:19-23](src/server.cpp#L19-L23)) or the handler returns `400` with an error JSON body.
3. **Business logic dispatch:** `rate_limiter_.check(client_id)` — [server.cpp:26](src/server.cpp#L26) → [src/rate_limiter.cpp:23-26](src/rate_limiter.cpp#L23-L26).
4. **Bucket lookup/creation:** `RateLimiter::getOrCreateBucket` — [rate_limiter.cpp:6-21](src/rate_limiter.cpp#L6-L21). Takes a shared (read) lock first to look up an existing bucket; only takes the exclusive (write) lock and calls `try_emplace` if not found (double-checked locking pattern, correctly implemented since `try_emplace` itself is race-safe under the exclusive lock).
5. **Token consumption:** `TokenBucket::consume(1.0)` — [src/token_bucket.cpp:19-38](src/token_bucket.cpp#L19-L38). Takes its own mutex, calls `refill()` ([token_bucket.cpp:8-17](src/token_bucket.cpp#L8-L17)) to lazily add tokens based on elapsed wall-clock time since last refill, then either debits a token (allowed) or computes a `retry_after_seconds` (rejected).
6. **Response:** handler builds `{allowed, remaining, retry_after}` JSON and sets HTTP status `200` (allowed) or `429` (rejected) — [server.cpp:28-35](src/server.cpp#L28-L35).

There is effectively only **one** user flow in this service — there's no second route to trace. (Metrics — see below — would be a second flow but is not wired to any endpoint.)

### Config load flow (secondary flow)

`main()` ([main.cpp:11](src/main.cpp#L11)) → `Config::loadFromFile` ([config.cpp:5-29](src/config.cpp#L5-L29)) → `YAML::LoadFile` + per-field optional overrides onto struct defaults ([config.hpp:5-19](src/config.hpp#L5-L19)) → any parse exception is caught and logged to stderr, silently falling back to hardcoded defaults ([config.cpp:25-27](src/config.cpp#L25-L27)) rather than failing startup.

### Data model

There is no database and no persistent schema. The only "data model" is in-memory:
- `RateLimiter::buckets_`: `std::unordered_map<std::string /*client_id*/, std::shared_ptr<TokenBucket>>` — [rate_limiter.hpp:20](src/rate_limiter.hpp#L20). One entry per distinct `client_id` ever seen, for the lifetime of the process.
- `TokenBucket` state: `capacity_`, `refill_rate_`, `tokens_`, `last_refill_` timestamp — [token_bucket.hpp:21-25](src/token_bucket.hpp#L21-L25). All in-process memory; nothing survives a restart.

### External dependencies / integrations

- **cpp-httplib** — embedded HTTP server, configured via `Config::server` (host/port) — [server.cpp:47](src/server.cpp#L47).
- **nlohmann/json** — request/response (de)serialization — [server.cpp:2,5](src/server.cpp#L2).
- **yaml-cpp** — config file parsing — [config.cpp:2](src/config.cpp#L2).
- **GoogleTest** — test framework only, not a runtime dependency.
- **Redis** — declared in config (`RedisConfig` struct, `config.yaml:11-14`) but **no Redis client library is linked anywhere** in `CMakeLists.txt`, and no code in `src/` references a Redis connection, `hiredis`, or similar. This is a **config-only stub** — see Phase 3.
- No database, no message queue, no cloud SDKs, no auth provider.

---

## PHASE 3 — CURRENT STATE

### Module classification

| Module | Classification | Notes |
|---|---|---|
| `config.hpp/.cpp` | **(a) Complete, working** — with a gap | Loads/parses YAML correctly with sane defaults and exception safety. **Gap:** no range validation — a `capacity <= 0` or `refill_rate <= 0` in `config.yaml` is accepted silently and will break the algorithm (see below). |
| `token_bucket.hpp/.cpp` | **(a) Complete, working** — with an edge-case bug | Core refill/consume math is correct and thread-safe (single mutex per bucket). **Bug:** `refill_rate_` of `0` causes division-by-zero at [token_bucket.cpp:34](src/token_bucket.cpp#L34) (`tokens_needed / refill_rate_`) → `retry_after_seconds` becomes `UINT64_MAX`-ish via `static_cast<uint64_t>` of `+inf`/`NaN` — undefined-behavior-adjacent and definitely a nonsensical API response. Not validated anywhere. |
| `rate_limiter.hpp/.cpp` | **(b) Partially implemented** | Locking is correct (shared/unique `shared_mutex`, `try_emplace` under exclusive lock avoids duplicate insert races). **Missing:** no eviction, TTL, or LRU/cap on `buckets_`. Every distinct `client_id` ever seen creates a permanent map entry for the life of the process — an **unbounded-memory / DoS vector** if `client_id` is attacker-controlled (e.g., derived from IP + header the caller controls) — see Phase 3 "production breakage" below. |
| `server.hpp/.cpp` | **(b) Partially implemented** | The one route it has (`/check`) is solid: validates input, correct status codes, catches JSON parse errors. **Missing:** no `/health` or `/metrics` endpoint, no request logging, no graceful shutdown wiring (`Server::stop()` at [server.cpp:50-52](src/server.cpp#L50-L52) is defined but **never called** from anywhere — `main.cpp` has no signal handler for SIGINT/SIGTERM), no CORS/security headers, no rate-limiting of the rate-limiter's own endpoint (a client could hammer `/check` and, per the point above, cheaply grow the bucket map with unique `client_id`s). |
| `metrices.hpp/.cpp` | **(c)/(d) Stubbed and dead code** | `Metrics` singleton with `incrementAllowed`/`incrementRejected`/`serialize` (Prometheus text format) is fully implemented in isolation — but **nothing in the codebase calls any of its methods**. Confirmed via repo-wide grep: the only references to `Metrics`/`incrementAllowed`/`incrementRejected` are inside `metrices.cpp`/`metrices.hpp` themselves. It is compiled into `rate_limiter_lib` ([CMakeLists.txt:57](CMakeLists.txt#L57)) but has zero callers and there is no `/metrics` HTTP route anywhere in `server.cpp`. This is scaffolding for a feature that was never finished/wired up — recent commits (`6334ecb`, `5a7106a`, `d8e8137`, `6973b29`) show it was added and fixed for a build error, but never connected to `Server::setupRoutes()`. |
| `redis` config (`RedisConfig`) | **(c) Stubbed / placeholder** | Parsed from YAML into a struct ([config.hpp:15-19](src/config.hpp#L15-L19), [config.cpp:20-24](src/config.cpp#L20-L24)) but **completely unused** downstream — no Redis client is linked or instantiated anywhere. This suggests a planned-but-unbuilt distributed-backend mode (the in-memory map obviously can't be shared across multiple service instances) — currently the service is **single-instance only** in practice, regardless of the `redis.enabled` flag, which is read but never checked anywhere outside `Config`. |
| `tests/rate_limiter_test.cpp` | **(b) Partially implemented (coverage gap)** | 4 tests, all for `TokenBucket`/`RateLimiter` business logic only (capacity limits, refill-over-time via a real 1.05s `sleep_for` — a flaky-under-load timing test, multi-client isolation, and a 10-thread × 10-req concurrency smoke test). **Zero tests** for `config.cpp` (YAML parsing/defaults/error path), `server.cpp` (HTTP status codes, malformed JSON, missing field, wrong types), or `metrices.cpp`. No test binary/harness exercises the actual HTTP server (e.g., no `httplib::Client` integration test). |
| `benchmarks/benchmark.cpp` | **(a) Complete, working** | Self-contained, not integrated into CI/CTest, but functionally sound as a manual perf tool. |
| `Dockerfile` / `docker-compose.yml` | **(a) Complete, working** (as far as can be verified without running it — build tooling wasn't available in this audit environment; see Phase 5) | Multi-stage build is a reasonable pattern; copies only the binary + config into the runtime image. |
| `CMakeLists.txt` | **(a) Complete, working**, with an operational caveat | Requires network access at *configure* time (all four deps are `FetchContent`-fetched from GitHub/git, not vendored) — no offline/air-gapped build path, no lockfile beyond the pinned git tags/URLs already in the file. |

### TODO / FIXME / HACK comments

**None found.** Repo-wide grep for `TODO|FIXME|HACK|XXX` across all `.cpp`/`.hpp`/`.txt`/`.yaml` files returned zero matches.

### Test coverage & pass status

- Could not execute the test suite in this audit environment — `cmake` is not installed and the available `g++` (MinGW 6.3.0) predates full C++17 support (e.g., `std::shared_mutex` requires C++17/glibc support not guaranteed on that toolchain) and there's no network-fetched dependency tree set up locally. **UNVERIFIED: whether `rate_limiter_test` currently passes.** To verify: install CMake ≥3.16 + a C++17 compiler (GCC ≥9/Clang ≥7/MSVC 2019+) with network access, then `cmake -B build && cmake --build build && ctest --test-dir build`.
- By code inspection, the tests look logically sound against the current implementation, **except**: `RefillsTokensOverTime` ([tests/rate_limiter_test.cpp:16-28](tests/rate_limiter_test.cpp#L16-L28)) sleeps a hardcoded 1050ms and assumes exactly 2 tokens refill — this is a real-clock-dependent test that can flake under CI/CPU contention (no tolerance margin, no clock injection/mocking in `TokenBucket`).

### Error handling gaps, hardcoded values, secrets

- **No hardcoded secrets found** (grepped for password/secret/api_key/token=/Authorization — no matches).
- **Hardcoded default config path** `"config.yaml"` as a relative path ([main.cpp:6](src/main.cpp#L6)) — works only if the process's CWD is the directory containing `config.yaml` (true in the Docker image since `WORKDIR /app` and both binary and config are copied there — [Dockerfile:19-21](Dockerfile#L19-L21) — but fragile for any other invocation style).
- **No input validation on config values** — `capacity`/`refill_rate` of 0 or negative are accepted and will corrupt behavior (division by zero, see above; negative capacity would make every request immediately rejected in a maybe-unintended way, untested).
- **No authentication/authorization** on `/check` — anyone with network access can query/consume any `client_id`'s bucket, and (per the unbounded-map issue) create unlimited new ones. This may be fine for a trusted internal sidecar but is undocumented as an assumption.
- **`Server::stop()` dead code path** — defined, never invoked; no signal handling in `main.cpp` means the process can only be killed externally (SIGKILL/SIGTERM without cleanup), not stopped gracefully via its own API.

### What would break in production

1. **Unbounded memory growth** — `RateLimiter::buckets_` never shrinks ([rate_limiter.hpp:20](src/rate_limiter.hpp#L20)); a service fronting many unique/rotating client IDs (or an attacker sending random `client_id` values) will leak memory until OOM-killed.
2. **No horizontal scaling** — state is entirely in-process memory; running more than one replica behind a load balancer gives each replica an independent, inconsistent view of every client's quota (defeats the purpose of the limiter). The `redis` config field implies this was intended to be solved, but it never was.
3. **No `/health` endpoint** — orchestrators (k8s, ECS, docker-compose healthchecks) have nothing to probe; `docker-compose.yml` defines no `healthcheck:` block either.
4. **No observability** — `Metrics` exists but is disconnected, so there is literally no operational visibility (request counts, latency, error rate) once deployed, despite the class existing.
5. **Divide-by-zero / bad config crash-adjacent behavior** if `rate_limit.refill_rate: 0` is ever set (accidentally or via bad automation) — see `token_bucket.cpp:34`.
6. **No graceful shutdown** — in-flight requests during a container stop/redeploy get hard-killed rather than drained (`Server::stop()` unused, no `SIGTERM` handling).

---

## PHASE 4 — WHAT'S LEFT

### Must-have (blocks a real production release)

| # | Item | Files touched | Effort | Depends on |
|---|---|---|---|---|
| 1 | Bound `RateLimiter::buckets_` — add TTL/LRU eviction of idle buckets | `rate_limiter.hpp/.cpp` | M | — |
| 2 | Validate config values (`capacity > 0`, `refill_rate > 0`, valid port range) at load time, fail fast or clamp with a loud warning | `config.cpp` | S | — |
| 3 | Guard `token_bucket.cpp:34` against `refill_rate_ <= 0` | `token_bucket.cpp` | S | Item 2 (validation may make this moot, but defense-in-depth) |
| 4 | Add `/health` (liveness) endpoint | `server.cpp` | S | — |
| 5 | Wire the existing `Metrics` singleton into `/check` (call `incrementAllowed`/`incrementRejected`) and expose a `/metrics` route | `server.cpp`, `metrices.cpp/.hpp` (rename typo optional) | S | — |
| 6 | Graceful shutdown: handle SIGINT/SIGTERM in `main.cpp`, call `Server::stop()` | `main.cpp`, `server.hpp/.cpp` | S | — |
| 7 | Decide & document the multi-instance story: either implement the Redis-backed shared limiter the config already gestures at, or explicitly document "single-instance only" and remove/relabel the unused `redis` config block | `config.hpp/.cpp`, `rate_limiter.*` (new Redis-backed impl), `CMakeLists.txt` (link redis client) | L | Item 1 (design interacts with eviction) |
| 8 | Finish the README (it currently cuts off mid-sentence in a fenced code block, no build/run/API docs at all) | `README.md` | S | — |
| 9 | Add CI (build + run tests on push/PR) | new `.github/workflows/ci.yml` | S | — |

### Should-have (quality / robustness)

| # | Item | Files touched | Effort | Depends on |
|---|---|---|---|---|
| 10 | Tests for `config.cpp` (defaults, malformed YAML, partial sections) | `tests/` (new file or extend existing) | S | — |
| 11 | Tests for `server.cpp` HTTP layer (400 on missing/invalid `client_id`, 400 on bad JSON, 429 on exhaustion) using an `httplib::Client` against a real listening instance or a test harness | `tests/` | M | — |
| 12 | Make the refill-timing test deterministic (inject a clock/fake time source into `TokenBucket` instead of `sleep_for`) | `token_bucket.hpp/.cpp`, `tests/rate_limiter_test.cpp` | M | — |
| 13 | Add `.gitignore` (currently none — a local `build/` dir per the README's implicit `mkdir build` workflow risks being committed) | new `.gitignore` | S | — |
| 14 | Request logging / structured logs | `server.cpp` | S | — |
| 15 | Rename `metrices` → `metrics` (filename + symbol hygiene) | `src/metrices.*` → `src/metrics.*`, `CMakeLists.txt` | S | Item 5 (do together) |
| 16 | Config reload without restart (SIGHUP or file-watch) — optional | `config.cpp`, `server.cpp` | M | Item 6 |

### Nice-to-have

| # | Item | Effort |
|---|---|---|
| 17 | Configurable token cost per request (currently hardcoded `1.0` at `rate_limiter.cpp:25`) | S |
| 18 | Per-client override of capacity/refill_rate (currently global only, from `config.yaml:7-9`) | M |
| 19 | Benchmark results wired into CI as a perf-regression gate | M |
| 20 | Structured error response schema / OpenAPI spec for the single endpoint | S |

### Prioritized build order & reasoning

1. **Items 2 → 3** (config validation, then the div-by-zero guard it enables) — cheapest correctness fixes, prevents silent corruption.
2. **Item 1** (bucket eviction) — the single biggest production-breaking gap (unbounded memory); do before exposing the service to any untrusted traffic.
3. **Item 6** (graceful shutdown) — cheap, and needed before real deployment/orchestration.
4. **Item 4** (`/health`) — cheap, required for any orchestrator.
5. **Item 5** (wire up Metrics) — the work is already done in isolation; just needs the two call-sites and a route. High value for low effort.
6. **Item 9** (CI) — lock in all of the above so regressions are caught automatically going forward.
7. **Item 8** (README) — do once the above land, so docs describe the real, finished behavior instead of needing a rewrite twice.
8. **Items 10–12** (test coverage) — backfill once behavior is stable.
9. **Item 7** (Redis/multi-instance) — largest, most architecturally significant item; explicitly last because it's optional scope (single-instance may be an acceptable permanent answer) and everything else should be solid first.
10. **Items 13–20** — polish, done opportunistically alongside the above.

---

## PHASE 5 — HOW TO RUN IT

None of this is documented in the repo (the README is unfinished — see Phase 3/4). Steps reconstructed from `CMakeLists.txt`, `Dockerfile`, and `docker-compose.yml`:

### Option A — Docker Compose (least assumptions, closest to "documented" since the compose file exists)

```sh
git clone <repo>
cd rate-limiter
docker compose up --build
```

- Exposes the service on `http://localhost:8080`.
- `config.yaml` is bind-mounted into the container ([docker-compose.yml:10-11](docker-compose.yml#L10-L11)), so edits to the local file take effect on container restart without rebuilding.
- **No env vars are used or required anywhere** — confirmed via source grep for `getenv`. All runtime config comes from `config.yaml`.

### Option B — Native build

```sh
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build . --config Release -j$(nproc)
./rate_limiter ../config.yaml     # or just ./rate_limiter if config.yaml is in CWD
```

Requirements (UNVERIFIED against this exact repo since the audit environment lacked `cmake` and a sufficiently modern compiler — could not actually execute a build):
- CMake ≥ 3.16 (stated minimum, [CMakeLists.txt:1](CMakeLists.txt#L1))
- A C++17 compiler (GCC/Clang/MSVC — none pinned or documented)
- **Network access at configure time** — `FetchContent` pulls 4 dependencies from GitHub on first `cmake ..` run; this is not called out anywhere and will silently fail/hang in an air-gapped environment.
- `pthreads` (via `find_package(Threads REQUIRED)`, [CMakeLists.txt:15](CMakeLists.txt#L15)) — present on Linux/macOS by default; UNVERIFIED on Windows/MSVC (the codebase uses `std::shared_mutex`/`std::mutex` from the standard library so it should be portable, but this hasn't been exercised here).

### Running tests

```sh
cd build
ctest --output-on-failure
```
(Registered via `gtest_discover_tests`, [CMakeLists.txt:80](CMakeLists.txt#L80).) **Not documented anywhere in the repo** — reconstructed from the build file.

### Running the benchmark

```sh
./build/benchmark
```
Also **not documented anywhere** — no README mention, no CLI args (hardcoded 8 threads × 100,000 requests, [benchmarks/benchmark.cpp:11-12](benchmarks/benchmark.cpp#L11-L12)).

### Calling the API once running

```sh
curl -X POST http://localhost:8080/check \
  -H "Content-Type: application/json" \
  -d '{"client_id": "test-client"}'
```
Also undocumented — reconstructed entirely from [server.cpp:14-42](src/server.cpp#L14-L42).

### Undocumented steps flagged

- The build/run/test/benchmark/API-call instructions above are **entirely absent from README.md**, which cuts off mid-code-block after line 21 with no closing fence and no further content.
- The network dependency of the CMake configure step is not mentioned anywhere.
- There is no documented way to enable/use the `redis` config block — and, per Phase 3, no code path actually consumes it if you did.

---

## Top 5 things I'd fix first

1. **Bound the client bucket map** ([src/rate_limiter.hpp:20](src/rate_limiter.hpp#L20)) — right now every distinct `client_id` lives forever in memory with no eviction. This is the single most likely cause of a production incident (slow OOM) and the fix (TTL sweep or LRU cap) is self-contained to `rate_limiter.hpp/.cpp`.
2. **Wire up the already-built `Metrics` class** ([src/metrices.cpp](src/metrices.cpp), unused) — call `incrementAllowed()`/`incrementRejected()` from [src/server.cpp:26-34](src/server.cpp#L26-L34) and add a `GET /metrics` route. The hard part is already written; leaving it disconnected means the service is currently unobservable in production for near-zero reason.
3. **Guard against zero/negative `refill_rate`/`capacity`** — [src/config.cpp](src/config.cpp) accepts any numeric value from YAML unchecked, and [src/token_bucket.cpp:34](src/token_bucket.cpp#L34) divides by `refill_rate_` with no zero-check. A single bad config value produces nonsensical `retry_after` values or crash-adjacent undefined behavior.
4. **Finish the README** — it currently ends mid-sentence inside an unclosed code fence ([README.md:20-21](README.md#L20-L21)) with zero build, run, Docker, test, or API-usage instructions. Every command in Phase 5 of this audit had to be reverse-engineered from `CMakeLists.txt`/`Dockerfile` instead of documented — that's a real onboarding cost for the next engineer.
5. **Add graceful shutdown + a health endpoint** — `Server::stop()` ([src/server.cpp:50-52](src/server.cpp#L50-L52)) is dead code with no signal handler calling it, and there's no `/health` route for orchestrators to probe. Both are small, well-understood additions that are prerequisites for running this safely under Docker/k8s.
