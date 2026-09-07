# Project Audit — Token Bucket Rate Limiter

Audit date: 2026-09-06. Scope: full repository, read-only review against actual source (not README claims).

> **Update — 2026-09-07/08: Stages 0–6 of this audit's build order have been executed.**
> Phases 1 and 2 below (map, purpose, data flow) are unchanged and still accurate. Phase 3
> (module classification), the "what would break in production" list, Phase 4 ("what's
> left"), and the "Top 5" section have been revised in place to reflect the current state —
> see the inline `**RESOLVED**`/`**STILL OPEN**` markers. Everything resolved was actually
> built, tested (17/17 GoogleTest cases passing), and in most cases manually verified against
> a running instance — not just asserted. Commits: `338d45b`..`900cd57` on `main`
> (`git log 6973b29..900cd57` for the full list). A genuinely new item — the Redis
> build-vs-remove decision — was analyzed and resolved in Stage 5; see the new section after
> Phase 4.

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

**RESOLVED** items below were fixed in Stages 1–5 (commits `338d45b`..`c381459`) and verified
by an actual build + 17/17 passing tests, plus manual runtime checks (curl, SIGTERM, real
`/metrics` scrapes) — see each commit message for exactly what was run.

| Module | Classification | Notes |
|---|---|---|
| `config.hpp/.cpp` | **(a) Complete, working** | **RESOLVED** (Stage 1, `338d45b`): `Config::loadFromFile` now validates every value at load time (`capacity`/`refill_rate`/`idle_ttl_multiplier`/`sweep_interval_seconds` > 0, `port` in `1..65535`, non-empty `host`) and throws `std::runtime_error` on a missing/malformed file or an invalid value; `main.cpp` catches it, prints `Fatal: ...`, and exits non-zero instead of starting misconfigured. Covered by 8 `ConfigTest` cases. |
| `token_bucket.hpp/.cpp` | **(a) Complete, working** | **RESOLVED** (Stage 1, `338d45b`): `refill_rate_ <= 0` no longer divides by zero — `consume()` reports `retry_after_seconds = UINT64_MAX` ("will never refill on its own") instead. The live service can't reach this state (`Config::validate` rejects it), but `TokenBucket` itself stays well-defined since it's also used directly by tests/benchmarks. Covered by `ZeroRefillRateNeverDividesByZero`/`NegativeRefillRateNeverDividesByZero`. |
| `rate_limiter.hpp/.cpp` | **(a) Complete, working** | **RESOLVED** (Stage 2, `08aec26`): idle-timeout eviction added (`EvictionConfig`, `sweepExpiredLocked`), configurable via `rate_limit.idle_ttl_multiplier`/`sweep_interval_seconds`. Piggybacks on the exclusive lock already taken on new-client insert, so the shared-lock read fast path is untouched (benchmark unaffected: ~9.9M req/sec, 0.29us p50, re-measured post-change). Lock-ordering with `TokenBucket::mutex_` and `Metrics::mutex_` documented inline; both are one-directional, no cycle. Covered by `EvictsIdleBucketsAndShrinksMap` and `MetricsTest.ActiveBucketsGaugeDropsAfterEvictionSweep`. |
| `server.hpp/.cpp` | **(b) Partially implemented** | **PARTIALLY RESOLVED.** `Server::stop()` is now wired up (Stage 3, `6dfa060`): `main.cpp` installs SIGINT/SIGTERM handlers (flag + watcher thread, async-signal-safe), manually verified to produce a clean exit with in-flight requests drained (confirmed from cpp-httplib's vendored source that `stop()` → `task_queue->shutdown()` drains the thread pool before `listen()` returns). `GET /metrics` added (Stage 4, `b5342e1`). **STILL OPEN:** no `/health` endpoint, no request logging, no CORS/security headers. |
| `metrices.hpp/.cpp` | **(a) Complete, working** | **RESOLVED** (Stage 4, `b5342e1`): every method is implemented and called from real code paths — `server.cpp`'s `/check` handler (`incrementRequestsReceived`, `incrementAllowed`/`incrementRejected`, a `LatencyRecorder` RAII guard covering every exit path) and `rate_limiter.cpp`'s insert/evict paths (`incrementActiveBuckets`, `incrementEvictedBuckets`/`decrementActiveBuckets`, kept in exact agreement with `buckets_.size()`). `GET /metrics` exposes it in Prometheus text format, `Content-Type: text/plain; version=0.0.4`. Verified against real captured output (see README's API section) — allowed+rejected vs. requests_received discrepancy explained, active_buckets matches actual client count. Internal thread-safety switched from mixed atomics+mutex to one mutex guarding all fields (documented why in `metrices.hpp`). **Filename typo (`metrices` vs. `metrics`) deliberately left as-is** — renaming touches `CMakeLists.txt` and both `#include` sites for a cosmetic-only change, out of scope for the stage that touched this file; still a valid future cleanup (see Phase 4). |
| ~~`redis` config (`RedisConfig`)~~ | **Removed** | **RESOLVED** (Stage 5, `c381459`) — removed rather than kept as a stub. See the new "Redis: build vs. remove" section below for the full analysis and reasoning. |
| `tests/rate_limiter_test.cpp` | **(b) Partially implemented (coverage gap)** | Grew from 4 to 17 tests across Stages 1, 2, and 4: config validation (8 cases), eviction (1), zero/negative refill-rate edge cases (2), and Metrics counters/gauge (2), alongside the original 4. **STILL OPEN:** no test exercises `server.cpp`'s HTTP layer itself (status codes, malformed-JSON handling) through an actual `httplib::Client` — that's currently only checked manually (see README's API section for the real curl transcripts from this session). The `RefillsTokensOverTime` real-clock `sleep_for` timing test is also still present and still a flake risk under load — not addressed in Stages 1–6. |
| `benchmarks/benchmark.cpp` | **(a) Complete, working** | Unchanged. Re-run post-Stage-2 to confirm the eviction-sweep change didn't regress the hot path: ~9.9M req/sec, 0.29us p50 (vs. ~9.45M/0.63us pre-change — within normal run-to-run noise). Still not integrated into CI/CTest (no CI exists at all — see Phase 4). |
| `Dockerfile` / `docker-compose.yml` | **(a) Complete, working** | **Verified this pass** (previously unverified — the original audit had no working build tooling). Found and fixed one real bug in the process: `Dockerfile`'s `COPY . .` had no `.dockerignore`, so a local `build/` directory (from building natively first, which the README's own instructions lead you to do) made the image build fail outright (`mkdir: cannot create directory 'build': File exists`). Fixed with a `.dockerignore` (separate commit, `8b39642`). `docker compose up --build`, `docker compose stop` (confirmed SIGTERM reaches the process as PID 1 and triggers the same graceful shutdown as the native binary), and `docker compose down` were all actually run and confirmed working end-to-end (built image, live `/check` and `/metrics` responses from the container, clean stop). Minor, not fixed: `docker-compose.yml`'s `version: '3.8'` attribute prints an "obsolete" warning under current Docker Compose — cosmetic, not functional. |
| `CMakeLists.txt` | **(a) Complete, working** | Unchanged; the network-at-configure-time caveat from the original audit still applies and is now explicitly documented in the README's Requirements section. |

### TODO / FIXME / HACK comments

**None found.** Repo-wide grep for `TODO|FIXME|HACK|XXX` across all `.cpp`/`.hpp`/`.txt`/`.yaml` files returned zero matches.

### Test coverage & pass status

**RESOLVED** — a working toolchain was set up this pass (Docker Desktop + an `ubuntu:22.04`
container with CMake 3.22.1 + GCC 11.4, matching the project's own `Dockerfile` base image,
since no local `cmake` and only a pre-C++17 MinGW `g++` were available directly). Build is
clean; **17/17 tests pass** (`ctest --output-on-failure`), re-confirmed after every stage
and again just before Commit F.

- `RefillsTokensOverTime`'s real-clock `sleep_for` flake risk (noted in the original audit) is
  **still present, not fixed** — out of scope for Stages 1–6, no stage touched it directly.

### Error handling gaps, hardcoded values, secrets

- **No hardcoded secrets found** (grepped for password/secret/api_key/token=/Authorization — no matches). Still true.
- **Hardcoded default config path** `"config.yaml"` — unchanged, still accurate, still low-risk (documented in README).
- ~~**No input validation on config values**~~ — **RESOLVED**, Stage 1 (`338d45b`). See the module table above.
- **No authentication/authorization on `/check`** — **STILL OPEN.** Unchanged from the original audit; not addressed by Stages 1–6.
- ~~**`Server::stop()` dead code path**~~ — **RESOLVED**, Stage 3 (`6dfa060`). See the module table above.

### What would break in production

1. ~~**Unbounded memory growth**~~ — **RESOLVED**, Stage 2 (`08aec26`): idle-timeout eviction, verified with a test that inserts 50 distinct clients and confirms the map shrinks back to 1 after a sweep.
2. **No horizontal scaling** — **STILL OPEN, by deliberate decision.** Stage 5 (`c381459`) analyzed what a Redis-backed shared limiter would take and recommended *not* building it in this pass, removing the misleading `redis` config stub rather than pretending it was closer to done than it was. See the new section below. Single-instance-only remains a real production constraint if this service is ever run as more than one replica.
3. ~~**No `/health` endpoint**~~ — **STILL OPEN**, not addressed by any of Stages 1–6 (not in their scope). `GET /metrics` (below) is the closest available liveness signal today.
4. ~~**No observability**~~ — **RESOLVED**, Stage 4 (`b5342e1`): `GET /metrics` in Prometheus format, covering requests received/allowed/rejected, active buckets, buckets evicted, and a request-latency histogram. Verified against real captured scrapes, not just asserted.
5. ~~**Divide-by-zero / bad config crash-adjacent behavior**~~ — **RESOLVED**, Stage 1 (`338d45b`).
6. ~~**No graceful shutdown**~~ — **RESOLVED**, Stage 3 (`6dfa060`), verified against both the native binary and the Docker container (SIGTERM in both cases produces the same clean-exit log lines).

---

## PHASE 4 — WHAT'S LEFT

*(Original items 1, 2, 3, 5, 6, 8, 13 are done — struck through with the resolving commit.
Item 7's decision is resolved — see the new Redis section below — but "implement a shared
backend" was explicitly rejected as out of scope, so multi-instance support itself remains
open. Item 15 (rename `metrices`→`metrics`) was deliberately left alone in Stage 4 to keep
that commit scoped to wiring, not a rename; it's carried forward below unchanged.)*

### Must-have (blocks a real production release) — what genuinely remains

| # | Item | Files touched | Effort | Depends on |
|---|---|---|---|---|
| ~~1~~ | ~~Bound `RateLimiter::buckets_`~~ | — | — | **DONE**, `08aec26` |
| ~~2~~ | ~~Validate config values at load time~~ | — | — | **DONE**, `338d45b` |
| ~~3~~ | ~~Guard `token_bucket.cpp:34` against `refill_rate_ <= 0`~~ | — | — | **DONE**, `338d45b` |
| 4 | Add `/health` (liveness) endpoint | `server.cpp` | S | — |
| ~~5~~ | ~~Wire `Metrics` into `/check`, expose `/metrics`~~ | — | — | **DONE**, `b5342e1` |
| ~~6~~ | ~~Graceful shutdown~~ | — | — | **DONE**, `6dfa060` |
| ~~7~~ | ~~Decide the multi-instance story~~ | — | — | **DECIDED** (remove, don't build), `c381459` — see Redis section below |
| ~~8~~ | ~~Finish the README~~ | — | — | **DONE**, `a8d8259`/`8ce9567`/`900cd57` |
| 9 | Add CI (build + run tests on push/PR) | new `.github/workflows/ci.yml` | S | — |

### Should-have (quality / robustness) — what genuinely remains

| # | Item | Files touched | Effort | Depends on |
|---|---|---|---|---|
| 10 | Tests for `config.cpp` | — | — | **DONE**, `338d45b` (8 `ConfigTest` cases) |
| 11 | Tests for `server.cpp` HTTP layer (400 on missing/invalid `client_id`, 400 on bad JSON, 429 on exhaustion) using an `httplib::Client` against a real listening instance | `tests/` | M | — |
| 12 | Make the refill-timing test deterministic (inject a clock/fake time source into `TokenBucket` instead of `sleep_for`) | `token_bucket.hpp/.cpp`, `tests/rate_limiter_test.cpp` | M | — |
| ~~13~~ | ~~Add `.gitignore`~~ | — | — | **DONE**, `922a7e3` (plus a matching `.dockerignore`, `8b39642`, found necessary while verifying Docker instructions) |
| 14 | Request logging / structured logs | `server.cpp` | S | — |
| 15 | Rename `metrices` → `metrics` (filename + symbol hygiene) | `src/metrices.*` → `src/metrics.*`, `CMakeLists.txt` | S | — |
| 16 | Config reload without restart (SIGHUP or file-watch) — optional | `config.cpp`, `server.cpp` | M | — |
| 21 | Fix `docker-compose.yml`'s obsolete `version: '3.8'` attribute (cosmetic warning under current Compose, found while verifying Stage 6 Docker instructions) | `docker-compose.yml` | S | — |

### Nice-to-have

| # | Item | Effort |
|---|---|---|
| 17 | Configurable token cost per request (currently hardcoded `1.0` at `rate_limiter.cpp:25` — now `RateLimiter::check()`'s call to `bucket->consume(1.0)`) | S |
| 18 | Per-client override of capacity/refill_rate (currently global only) | M |
| 19 | Benchmark results wired into CI as a perf-regression gate | M |
| 20 | Structured error response schema / OpenAPI spec for the endpoints | S |

### Prioritized build order for what's left

1. **Item 4** (`/health`) — cheap, still the most-requested missing piece for any real orchestrator deployment; the natural next step now that shutdown/metrics exist.
2. **Item 9** (CI) — nothing currently re-runs the 17 tests automatically on push/PR; the biggest risk to everything just fixed is silent regression.
3. **Item 11** (HTTP-layer tests) — the API is now documented and manually verified (see README), but still has zero automated coverage of the routing/status-code layer itself.
4. **Item 12** (deterministic refill test) — small, standalone, removes the one known flake risk.
5. **Items 14, 15, 16, 21** — polish, no dependencies between them, pick up opportunistically.
6. **Items 17–20** — genuinely optional feature work, lowest priority.

---

## REDIS: BUILD VS. REMOVE (Stage 5 decision)

`RedisConfig` (formerly `config.hpp:15-19`) was parsed from `config.yaml` and validated at
startup, but nothing anywhere branched on `redis.enabled` — the service only ever used the
in-memory `RateLimiter`. This section records the analysis behind removing it (`c381459`)
rather than implementing it or leaving it as a "reserved" stub.

**What actually implementing a Redis-backed shared limiter would involve:**

1. **Client dependency** — a C++ Redis client (e.g. `redis-plus-plus`, wrapping `hiredis`).
   Two new `FetchContent` dependencies, and unlike this project's current deps, a compiled C
   library in the chain rather than mostly header-only.
2. **Atomic token-bucket via Lua script** — `GET`+`SET` across two round trips isn't atomic;
   the standard pattern is a Lua script run via `EVAL`/`EVALSHA` (Redis executes scripts
   atomically). The script reads `{tokens, last_refill}` from a hash keyed by `client_id`,
   computes refill using a timestamp *passed in by the client* (Lua's own clock isn't safe to
   trust across replicas), debits tokens, writes back, and sets a TTL on the key — which
   would replace Stage 2's manual idle-sweep entirely for this backend.
3. **Backend abstraction** — `RateLimiter::check(client_id)` is the natural seam: it'd become
   an interface with `InMemoryRateLimiter` (today's code) and `RedisRateLimiter` as two
   implementations, selected in `Server`. This also means the Stage 4 `active_buckets`/
   `evicted_buckets` metrics stop making sense as-is for the Redis backend (no local map to
   introspect — would need Redis `DBSIZE`/`SCAN`, itself an anti-pattern at scale).
4. **Unreachable-Redis behavior — a real trade-off, not a detail:** fail open (allow requests
   when Redis is down — availability over protection, but rate limiting silently vanishes
   during exactly the outages that correlate with attack/overload traffic) vs. fail closed
   (reject requests when Redis is down — protection over availability, but the rate limiter
   becomes a single point of failure for everything behind it). The credible production
   answer is usually a bounded circuit-breaker with metrics/alerting on the degraded state,
   meaningfully more code than either extreme.
5. **Everything else this drags in** — connection pooling/thread-safety for the httplib
   worker pool, connect/command timeouts (which would cap `/check`'s own worst-case latency
   and invalidate Stage 4's histogram bucket boundaries, tuned for a pure in-memory hot
   path), auth/TLS (the removed `RedisConfig` had no password field — real Redis deployments
   need one), and test infrastructure (a real or containerized Redis in CI, which doesn't
   exist in this repo at all). Realistic throughput also drops by orders of magnitude from
   the ~9.9M req/sec in-memory benchmark once every check is a network round trip.

This is genuinely large, multi-part work — matching the original audit's own "L" estimate for
item 7. **Not implemented in this pass**, by design.

**Decision: remove `RedisConfig`, don't keep it as a "reserved, unimplemented" stub.**

Reasoning: it wasn't neutral dead code — `redis.enabled` parsed and validated cleanly and
*looked* like a working switch, so an operator could plausibly set `redis.enabled: true`,
restart, and observe zero behavior change while believing they'd enabled shared
multi-instance state. That's an active foot-gun, and it's the exact failure mode item 2 under
"what would break in production" already flagged. A "reserved, unimplemented" comment next to
a boolean that's fully wired into YAML parsing and startup validation is a weak signal —
comments get skipped; a key that parses without error gets flipped in real configs by people
who never read the C++ source. It also wouldn't have saved future work: a real implementation
needs a password field, timeouts, and probably TLS options this struct didn't have, plus the
backend-abstraction layer and Lua-script atomicity described above — none of which the
three-field struct provided a head start on. If/when a shared backend is actually built, it
should be designed from these real requirements, not retrofitted onto three placeholder
fields.

---

## PHASE 5 — HOW TO RUN IT

**Superseded by the README, which is now accurate and current — treat it as the source of
truth for run instructions rather than this section.** The original Phase 5 here was written
when the README was unusable (cut off mid-sentence, no build/run/API docs at all) and every
command had to be reverse-engineered from `CMakeLists.txt`/`Dockerfile`/`docker-compose.yml`
without being able to actually run them (no working toolchain in that environment). That gap
is closed: this pass set up a working toolchain, actually ran every command, and wrote the
results into the README (see Commits C, D, E) rather than duplicating them here — keeping the
same instructions in two places would just let them drift out of sync again. See:

- README's *Building and running locally*, *Running with Docker*, *Testing*, *Benchmarking*
  sections for exact commands (all actually run this session).
- README's *Configuration* section for the full YAML reference and startup-validation rules.
- README's *API* section for `POST /check` and `GET /metrics`, with real captured
  request/response transcripts.

One correction to the original Phase 5's own claims, found while re-verifying it this pass:
`docker compose up --build` did **not** just work as originally assumed it would — it failed
(`mkdir: cannot create directory 'build': File exists`) because the `Dockerfile`'s `COPY . .`
had no `.dockerignore` to stop it from copying a local `build/` directory into the image. Fixed
with a `.dockerignore` (`8b39642`) rather than documented as a limitation, since it was a real,
fixable bug, not an inherent constraint.

---

## What genuinely remains

Everything in the original "Top 5 things I'd fix first" is resolved: bucket eviction, Metrics
wiring, config validation, the README, and graceful shutdown are all built, tested, and
manually verified (see Phase 3/4 above for exact commits). What's left, in priority order:

1. **Add CI** (Phase 4, item 9) — the 17 tests only run when someone remembers to run them
   locally. This is the single biggest risk to everything else on this list staying fixed.
2. **Add a `GET /health` endpoint** (item 4) — `GET /metrics` is a usable stand-in today, but
   a dedicated liveness route is what most orchestrators actually expect to probe.
3. **Automated HTTP-layer tests** (item 11) — the routing/status-code layer (`server.cpp`) is
   currently verified only by manual curl transcripts (real ones, captured this pass — see
   README — but not regression-tested).
4. **Multi-instance / shared state** — deliberately not built (see the Redis section above).
   Still the right call unless/until there's an actual requirement for more than one replica;
   revisit with the real analysis above, not from scratch.
5. **Everything else in Phase 4's should-have/nice-to-have tables** — deterministic refill
   test, request logging, the `metrices`→`metrics` rename, config hot-reload, per-client
   overrides, the `docker-compose.yml` version-attribute warning — genuine but lower-stakes
   cleanup, none blocking a release on their own.
