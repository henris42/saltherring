# The server test system — container-based system tests for all backends

Originally the plan for real-database testing (written 2026-09-15; phases
0–2 executed the same day, then extended with Oracle per S-15). This file
now documents the system as it exists. It is an **independent layer**: the
unit suite (`test_saltherring` + negative-compile proofs) needs nothing
but the compiler, while the server suites are system tests that run the
real client library against a real server in a container — and skip
cleanly when no server is reachable, so plain `ctest` works everywhere.

## How to run it

```
scripts/server-tests.sh            # compose up --wait → ctest -L server → down -v
scripts/server-tests.sh --keep     # leave the containers running (debugging)
```

or by hand:

```
docker compose -f tests/containers/docker-compose.yml up -d --wait
ctest --test-dir build -L server --output-on-failure
docker compose -f tests/containers/docker-compose.yml down -v
```

Current state: **9/9 green** — unit suite, `pg`, `mariadb`, `oracle`, and
five negative-compile proofs, against postgres:17-alpine, mariadb:11.4 and
gvenzl/oracle-free:23-slim.

## Architecture

Test binaries build on the **host** (GCC 16.1 with reflection lives only
here) against the client libraries; servers run in **containers** on
non-default loopback ports so a developer's own local servers are never
touched. Host requirements per backend are user-facing docs now:
user-guide.md § Backend setup.

```
tests/containers/docker-compose.yml   postgres :15432 · oracle :11521 · mariadb :33069
tests/server_fixture.hpp              env config, EXPECT macros, skip(77)
tests/test_pg.cpp                     conformance + pg-specific suite
tests/test_mariadb.cpp                conformance + mariadb-specific suite
tests/test_oracle.cpp                 conformance + oracle-specific suite
tests/conformance.hpp                 the driver contract, shared with the unit suite
scripts/server-tests.sh               one-shot runner
```

(Postgres port is 15432, not the 543xx range — high ports collide with
WSL2/winnat phantom reservations.)

### The contract between ctest and the suites

- Connection info from env vars, defaults matching the compose file:
  `SALT_PG_DSN`, `SALT_MARIADB_HOST/PORT/USER/PASSWORD`,
  `SALT_ORACLE_CONNECT` / `SALT_ORACLE_SYSTEM_PASSWORD`.
- Unreachable server → `exit 77` → ctest **SKIP** (`SKIP_RETURN_CODE 77`,
  label `server`). Never a failure.
- Per-run isolation, so reruns never see stale state: a scratch schema
  (Postgres), scratch database (MariaDB), or scratch user (Oracle), each
  named `salt_run_<pid>` and dropped on exit. `down -v` resets everything
  between sessions regardless.

### What each suite covers

All three: the full conformance contract, the migration lifecycle with
tamper refusal, `execute()` affected-row counts, lock conflicts as coded
errors (never hangs). Then per dialect: pg — RETURNING ids, live `$n`/`??`
adaptation, Oid text shaping, transactional-DDL atomicity; mariadb —
AUTO_INCREMENT/`last_insert_id`, matched-vs-changed rows, multi-statement
draining, DDL-self-commit caveat; oracle — RETURNING…INTO ids, `''` IS
NULL semantics, PL/SQL through `exec()`, NUMBER shaping, DDL-self-commit
caveat, FOR UPDATE NOWAIT.

## What this layer has caught (the case for it)

Every one of these passed unit tests and died against a real server:

- MariaDB `MYSQL_BIND` buffer pointers dangling after a param-vector
  resize — **garbage silently stored** (history rows with corrupt version
  numbers that often happened to read back correctly on the allocator's
  whim).
- MariaDB multi-statement `exec` swallowing later statements' errors — a
  **failed migration recorded as applied**.
- Empty string/blob binding as NULL (MariaDB; same class as the SQLite
  empty-blob bug the conformance suite caught locally).
- MariaDB `TEXT`/`BLOB` 64 KiB cap → dialect now LONGTEXT/LONGBLOB.
- Postgres NUL-in-TEXT silent truncation → refused at bind.
- Oracle identifier folding: quoted-lowercase columns unreachable from
  unquoted tails → `dialect.fold_upper`, quoted-UPPERCASE identifiers.
- Oracle has no `RELEASE SAVEPOINT` → driver no-op.

Engine-semantics differences are **carve-outs inside conformance.hpp**,
keyed on `db.dial().name`, each with a comment stating the engine fact:
NUL-in-TEXT (Postgres refuses), `''` IS NULL (Oracle). The default path
stays strict for every other backend.

## Known quirks

- Oracle cold start: after `down -v` the database is rebuilt and the
  `FREEPDB1` service registers ~1 min *after* the container reports
  healthy; a suite run in that window skips. Rerun passes. (Tightening
  the compose healthcheck to probe FREEPDB1 is a welcome improvement.)
- Oracle runtime linking (RPATH transitivity, the Ubuntu 24.04
  `libaio.so.1t64` symlink) — documented in user-guide.md § Backend
  setup; the test binary handles it via `--disable-new-dtags` + the
  client-dir symlink.

## Remaining work — phase 3: CI

Not started. The shape agreed in the original plan:

- `docker/toolchain.Dockerfile`: GCC 16.1 + libpq-dev + libmariadb-dev +
  Instant Client + a sardine checkout, pushed to GHCR once (~60–90 min
  build, then cached by digest).
- `.github/workflows/ci.yml`: unit suite + all three server suites via
  `services:`, reusing the same binaries and env-var contract.
- Version-floor matrix as a documented support statement — proposal:
  `postgres:14`, `mariadb:10.11` (LTS), oracle-free 23ai (already the
  floor: the dialect emits 23c+ SQL). SQLite's floor (3.37) is enforced
  in code.
- Acceptance: a PR touching a driver cannot merge green without passing
  conformance against its live server.
