# container architecture

## Current production path

`container/` is now an own-developed C++ RandomX compute runtime. It no longer wraps an upstream runtime binary and no longer builds the old fake prototype.

```text
Dockerfile
├─ build stage
│  ├─ build local C++ code: edge-runtime
│  └─ FetchContent RandomX reference library v2.0.1
└─ runtime stage
   ├─ /usr/local/bin/edge-runtime
   ├─ /app/start.sh
   └─ /app/reporter/index.js

Runtime
├─ edge-runtime
│  ├─ Stratum TCP/TLS client
│  ├─ RandomX worker threads
│  ├─ share submission accounting from pool responses only
│  └─ V10 HTTP summary API on :8081 (/1/summary)
└─ reporter process
   ├─ polls 127.0.0.1:8081/1/summary
   ├─ local /health and /stats on :8080
   └─ optional REPORTER_ENDPOINT push, disabled by default
```

## V10 compatibility

The V10 Durable Object contract requires:

- container readiness port: `8081`
- metrics endpoint: `GET /1/summary`
- summary fields: `hashrate.total`, `results.shares_good`, `results.shares_total`, `connection.uptime`, `connection.pool`, `algo`, and `uptime`

`edge-runtime` provides those fields directly. The optional Node reporter is only a local health/stats helper and supervisor trigger.

## Correctness boundaries

Implemented in local code:

1. Stratum URL parsing and TCP/TLS connection.
2. JSON-RPC `login`, initial job extraction, async `job` notification handling.
3. JSON-RPC `submit` and `keepalived` emission.
4. Accepted/rejected counters driven by pool responses, not local guesses.
5. RandomX cache/dataset/VM lifecycle through the reference C API.
6. Monero-style nonce insertion at blob offset 39.
7. V10-compatible `/1/summary` API for Worker polling.

External primitive dependency:

- RandomX reference library: `https://github.com/tevador/RandomX`
- Pinned source: `v2.0.1` / `aaafe71322df6602c21a5c72937ac284724ae561`

Behavior/API references are design-time only and are not built into the image.

## Remaining validation

Build validation:

```bash
docker build --platform linux/amd64 -t container-custom-randomx:local .
```

Runtime API validation:

```bash
curl -fsS http://127.0.0.1:8080/health
curl -fsS http://127.0.0.1:8081/1/summary
```

Runtime correctness validation requires an accepted share from a live pool or controlled Stratum test endpoint. A non-zero local hashrate alone is not enough.
