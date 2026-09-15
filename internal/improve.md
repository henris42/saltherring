# saltherring — Improvement Notes

Source: read-only review of `henris42/saltherring` at commit `9262310`
(`include/saltherring/*.hpp`, `tests/test_saltherring.cpp`). Not built
(no GCC 16 in the review environment). Line numbers refer to that commit.

Audience: coding agent or engineer. Each note has an id, priority, the
location, the problem, the fix, and the test that proves it. Work in id
order; S-1 changes the public API and should land first so the others
build on it.

**Design principle.** saltherring is a generic ORM, not a CA component.
Every note below adds a *mechanism* with a default that suits a general
user; the strict *policy* (which mode, which overloads are banned, which
options are mandatory) lives in the application and is enforced there
by lints, negative-compile tests, and wrapper types. Nothing here should
remove a capability a generic user reasonably wants. Where a note says
"storax-ca does X", that is application configuration, not library
behaviour.

Priorities: **P0** — silent data corruption or an unsafe default;
**P1** — correctness under concurrent or hostile use; **P2** — hardening
and hygiene.

---

## S-1 · P0 · Literal-only SQL tail type (additive overloads)

**Where**: `saltherring.hpp` — `db::exec` (744), `db::scalar` (758),
`db::query` (831), `db::query_one` (844), `db::count` (883).

**Problem**: All take a runtime `std::string_view` tail, which is right
for a generic ORM but gives a security-sensitive application no way to
make runtime-built SQL uncompilable. The library should offer a
literal-only tail type *alongside* the existing overloads; an
application that wants the guarantee bans the `string_view` overloads
with its own lint.

**Fix**:
```cpp
// A SQL fragment that can only come from a literal.
struct sql {
  consteval sql(const char* s) : text(s) {}     // literal only
  std::string_view text;
};
// Overloads take `sql` instead of std::string_view. Keep a single
// escape hatch, deliberately ugly and greppable:
struct unchecked_sql { std::string_view text; };
```
- `query<T>(sql tail, args...)`, `count<T>(sql tail, ...)`, `exec(sql, ...)`,
  `scalar<V>(sql, ...)`, `query_one<T>(sql, ...)`.
- `migration::sql` / `::down` stay `string_view` (they are constexpr
  tables already); `migration::fn` code migrations use `db&` normally.
- Keep the existing `std::string_view` overloads unchanged for generic
  users; `sql` overloads are additive. `unchecked_sql` is for callers
  that want to be explicit about runtime SQL. (storax-ca: lint bans the
  `string_view` and `unchecked_sql` overloads outside its tenant-scope
  module.)

**Test**: `db.query<User>(salt::sql{std::string(...)})` fails to
compile; literal `sql` tails work; `string_view` tails still work.
(storax-ca's T-build-4 tests its own ban, not the library.)

---

## S-2 · P0 · Reject or correctly store 64-bit unsigned members

**Where**: `detail::to_sql` (409–410), `detail::from_sql` (453–467).

**Problem**: `static_cast<std::int64_t>(v)` for any integral. A
`std::uint64_t` above `INT64_MAX` stores negative; on read
`std::in_range<M>(i)` fails with `errc::out_of_range`. Half of all random
64-bit certificate serials would be unreadable if declared `uint64_t`.

**Fix** (choose one, document it):
1. `static_assert(!(std::unsigned_integral<U> && sizeof(U) == 8), "…store as BLOB or text")` in `kind_of<M>()`. Simplest, matches the "no silent narrowing" spirit.
2. Map unsigned 64-bit to `col_kind::blob` (8 big-endian bytes) or text
   decimal, round-tripping exactly. More work, more convenient.

Also reject `__int128` and `unsigned long` where `sizeof == 8`
(platform-dependent aliases).

**Test**: round-trip `std::uint64_t{0xFFFF'FFFF'FFFF'FFFF}` and
`0x8000'0000'0000'0000`; either fails to compile (option 1) or reads back
equal (option 2). Add `std::uint32_t` max and `std::int8_t` min/max to
the existing integer tests.

---

## S-3 · P0 · Stored down-SQL execution and checksum coverage

**Where**: `detail::checksum` (1071–1081), `detail::unwind_one`
(1111–1120), `migrate` unwind path (1157–1175), `rollback` (1229–1242).

**Problem**: `down_sql` is stored verbatim in `salt_schema_history` and
executed from the database on `rollback()` and `unwind_missing`. Write
access to that table becomes SQL execution as the runtime role at the
next migrate/rollback. The checksum covers only the up SQL, and FNV-1a
is drift detection, not tamper evidence.

**Fix**:
- Extend the checksum to cover `version || description || up || down`
  (keep FNV-1a for compatibility, bump a `checksum_version` column so
  old rows validate under the old rule once, then are rewritten).
- Before executing a stored `down_sql`, recompute the checksum from the
  stored row and compare; refuse with a new `errc::migration_tampered`
  on mismatch.
- `unwind_missing` stays available; it is a legitimate development
  workflow. Add `migrate_options::verify_down_checksum = true` so a
  tampered stored `down_sql` is refused by default, and document that
  applications with forward-only production schemas should simply not
  call `unwind_missing`/`rollback` there.
- Document: a cryptographic tamper-evidence layer (signed history) is
  the application's job; storax-ca must not reuse this checksum for its
  audit chain.

- **Migration atomicity per dialect.** Add a dialect capability flag
  `transactional_ddl` (true: Postgres, SQLite; false: MySQL/MariaDB —
  any DDL statement commits implicitly and cannot be rolled back, the
  same as Oracle). `migrate()` uses one of two paths:
  - *atomic*: DDL and the history row in one transaction;
  - *recoverable*: insert the history row first with
    `state = 'started'`, run the migration statement by statement, then
    update to `state = 'applied'`. On the next `migrate()` a `started`
    row means a crash mid-migration: refuse to proceed and report which
    migration is half-applied, unless the migration is marked
    `idempotent = true` (its SQL uses `IF NOT EXISTS` / `IF EXISTS`
    forms), in which case re-run it. Add `state` to
    `salt_schema_history` (default `applied` for existing rows).
  MySQL 8.0's atomic DDL only makes a single DDL statement
  all-or-nothing; it does not enlist DDL in the caller's transaction.

**Test**: apply v1–v2, edit `down_sql` of v2 directly via `raw()`, call
`rollback(db, 1)` → `migration_tampered`, table unchanged; same for
`unwind_missing`. On a non-`transactional_ddl` dialect (conformance
suite, S-12): kill between DDL and history update → next `migrate()`
refuses with the half-applied version named; same migration marked
idempotent → re-run succeeds.

---

## S-4 · P0 · Exception-safe transactions, savepoints, isolation; opt-in scoped mode

**Where**: `db::transaction` (897–906); every statement path in `db`.

**Problem**: if `f()` throws, no `ROLLBACK` runs and the connection stays
mid-transaction. No savepoints, no isolation control. And there is no
mode in which an application can insist that every statement runs
inside a transaction it controls.

**Fix — generic part (default behaviour)**:
- `transaction(f, tx_options)` with an RAII guard: rollback on error
  return *and* on unwind. `tx_options{ isolation, immediate, read_only }`
  rendered per dialect (`BEGIN IMMEDIATE` on SQLite,
  `BEGIN ISOLATION LEVEL …` on Postgres).
- Nested `transaction()` calls use `SAVEPOINT sp_<depth>` /
  `RELEASE` / `ROLLBACK TO`; depth tracked in `db`.
- Serialization failure (Postgres `40001`, SQLite `SQLITE_BUSY` on
  lock upgrade, MySQL/MariaDB deadlock `1213` and lock-wait timeout
  `1205`) surfaces as `errc::retryable`; the library never retries.
- **Nesting must never reach the server as a second `BEGIN`.** On
  MySQL/MariaDB, `START TRANSACTION` inside an open transaction
  implicitly commits the outer one instead of nesting or erroring.
  The depth counter in `db` is therefore the only thing standing
  between a nested call and a silent partial commit: depth 0 → `BEGIN`,
  depth ≥ 1 → `SAVEPOINT`, always. Assert this in the driver interface
  (a driver receives `begin()` only at depth 0).
- Drivers set session defaults explicitly on connect: MySQL/MariaDB
  `SET autocommit = 0` (default is on) and
  `SET SESSION TRANSACTION ISOLATION LEVEL …` per `tx_options`;
  Postgres `SET default_transaction_isolation` likewise. Never rely on
  server defaults.
- Isolation note for the docs: MySQL `SERIALIZABLE` turns plain reads
  into locking reads (deadlock-prone under parallel writers), Postgres
  uses serializable snapshot isolation. Both satisfy the guarantee;
  applications with parallel writers on MySQL should expect more
  `retryable` results.
- Expose the same guard as an RAII object for callers that prefer it:
  ```cpp
  [[nodiscard]] result<scope> begin(tx_options o = {});
  // scope: move-only; commit() or destructor rollback.
  ```

**Fix — opt-in `db_mode::scoped`** (for applications that want
"one unit of work, one transaction, owned by a dispatcher"):
- In this mode a statement executed with no open scope is a contract
  violation, and nested `begin()` is a contract violation (no
  savepoints; the boundary is the unit of work).
- The library does not decide who owns the scope. An application that
  wants handlers unable to open one hides the `scope` type behind its
  own dispatcher (storax-ca does this; see enterprise plan §0.1).
- Migrations open one scope per migration (DDL + history row) in both
  modes.

**Test**: `transaction([]{ throw …; })` leaves the connection usable and
no partial rows; nested savepoint rolls back the inner unit only, and on
MySQL/MariaDB the conformance suite proves the outer unit was *not*
committed by the inner one; a driver receiving `begin()` at depth > 0
trips a contract;
`BEGIN IMMEDIATE` blocks a second writer; in `scoped` mode a bare
statement trips the contract; kill -9 between two inserts in one scope →
neither row after restart; a migration failing on its second statement
leaves no history row.

---

## S-5 · P1 · Refuse trailing statements in `prepare`

**Where**: `sqlite::connection::prepare` (128–134); `pg::connection::prepare`
(160–162) — libpq `PQexecParams` already rejects multiple statements, keep
it that way in any custom driver.

**Problem**: `sqlite3_prepare_v2(..., nullptr)` prepares only the first
statement; `"UPDATE …; DROP TABLE …"` silently drops the rest.

**Fix**: pass `&tail`; if the remainder is not whitespace/comments,
finalize and return `errc::prepare` "multiple statements in prepare; use
exec()". Same rule in the driver interface docs for all backends.

**Test**: `db.exec("UPDATE users SET name='x'; DELETE FROM users", 0)`
with args → error, no rows changed.

---

## S-6 · P1 · Placeholder rewriting needs a tokenizer

**Where**: `detail::adapt_placeholders` (556–569).

**Problem**: toggles on every `'`, ignores `--` and `/* */` comments,
double-quoted identifiers, backtick identifiers, Postgres `$$…$$` /
`$tag$…$tag$` strings, `E'…'` escapes, and rewrites the Postgres JSON
`?`, `?|`, `?&` operators into `$n`.

**Fix**: a small state machine over the tail: states
`code | squote | dquote | bquote | line_comment | block_comment | dollar_quote(tag)`.
Rewrite `?` only in `code`. Provide `??` as an escape meaning a literal
`?` in code (document it; Postgres JSON users need it). Reject unbalanced
quotes with `errc::prepare` rather than guessing.

**Test**: table-driven cases for every state; `WHERE data ?? 'k'` on
`postgres_dialect` yields `data ? 'k'`; `'it''s ?'` and `"col?"` untouched;
`-- ?\nWHERE x = ?` → `$1` only once.

---

## S-7 · P1 · Hardened SQLite open

**Where**: `sqlite::open` (172–185); `sqlite.hpp` ABI shim (11–49).

**Problem**: `sqlite3_open` with no flags: URI filenames honored if the
library enables them, symlinks followed, no busy timeout, default
journal, no `secure_delete`, no defensive mode, extensions loadable,
system `libsqlite3.so.0` of unknown version.

**Fix**:
- `open(const char* path, sqlite_options o = {})` using `sqlite3_open_v2`
  with `SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOFOLLOW`
  (drop `CREATE` when `o.create == false`; add `SQLITE_OPEN_READONLY`
  option); never `SQLITE_OPEN_URI` unless requested.
- After open: `sqlite3_extended_result_codes(c, 1)`,
  `sqlite3_db_config(c, SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr)`,
  `SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION = 0`,
  `sqlite3_busy_timeout(c, o.busy_ms)` (default 5000),
  `PRAGMA journal_mode = WAL` (option, default off — file-layout
  change), `PRAGMA synchronous` (option, default `NORMAL`; applications
  that need durability over throughput choose `FULL`),
  `PRAGMA secure_delete` (option, default off — it costs write
  throughput; storax-ca turns it on), `PRAGMA trusted_schema = OFF`,
  keep `PRAGMA foreign_keys = ON`. Defensive mode and no-extension-load
  are on by default: they only remove footguns.
- Add the missing declarations to the ABI shim (`sqlite3_open_v2`,
  `sqlite3_db_config`, `sqlite3_busy_timeout`,
  `sqlite3_extended_result_codes`, `sqlite3_libversion_number`) and
  refuse to open below a minimum `sqlite3_libversion_number()` (pick a
  version and document why).
- Application note (storax-ca): vendor the amalgamation compiled with
  `SQLITE_SECURE_DELETE`, `SQLITE_OMIT_LOAD_EXTENSION`,
  `SQLITE_DEFAULT_FOREIGN_KEYS=1`, `SQLITE_DQS=0`; don't depend on the
  container's shared library.

**Test**: open a symlinked path → error; open with `create=false` on a
missing file → error; `PRAGMA secure_delete` reads back 1; extension
load call fails.

---

## S-8 · P1 · Keep values out of error messages by default

**Where**: `enum_from_text` (396–397), `from_sql` range check (464–465),
`error::sql` populated in `prepare`/`exec`/`scalar` (132, 144, 150, 765).

**Problem**: messages embed the offending value; `error.sql` embeds the
full statement. Errors flow into logs.

**Fix**:
- `salt::error` gains `std::string detail` (may contain values) separate
  from `message` (never does); `message` says "value is not an
  enumerator of Level", `detail` carries the text.
- Cap `detail` and `sql` at a configurable length (default 512) and add a
  `[[=salt::sensitive{}]]` member annotation: for such columns, `detail`
  is left empty and the error notes "(redacted)". Both default to the
  current behaviour (values present) so generic users keep useful
  diagnostics; `sensitive` is opt-in per column.
- Document that `error.sql` never contains bound values (true today —
  keep it true; add a test).

**Test**: decoding a bad enum for a `sensitive` column produces an error
whose `message`+`detail`+`sql` do not contain the stored text.

---

## S-9 · P1 · Driver hygiene in `pg.hpp` (or in the replacement driver)

**Where**: `pg::statement::column` (89–115).

**Problem**: `std::from_chars` results ignored → malformed numeric text
silently becomes 0; bytea hex decoder accepts non-hex characters and odd
lengths silently; `oid_numeric` is truncated to `double` without warning.

**Fix**: check `ec == std::errc{}` and full consumption, else
`errc::type_mismatch`; validate hex and even length; treat `numeric` as
text unless the member is floating-point (or add a `col_kind::decimal`
later). Apply the same rules to storax-ca's own wire-protocol driver;
add a driver conformance test (see S-12) that any `backend::connection`
must pass.

**Test**: the conformance suite feeds `"12abc"` for an int8 column and
`"\\xZZ"` for bytea and expects errors.

---

## S-10 · P2 · Consteval identifier validation

**Where**: `table_name` (245–250), `column_name` (252–256),
`build_columns` (282–314), `references` (96–101).

**Problem**: annotation strings are compile-time constants, but nothing
rejects a quote character, whitespace, or an empty name; `quoted()`
(539–543) does not escape.

**Fix**: a `consteval bool valid_identifier(std::string_view)` allowing
`[A-Za-z_][A-Za-z0-9_]*`, max 63 chars (Postgres limit); `throw` in
`build_columns` / `table_name` on violation. Also reject a `check`/
`sql_default` expression containing a NUL byte.

**Test**: negative-compile file with `[[=salt::table("us\"ers")]]`.

---

## S-11 · P2 · Timestamps and time zones

**Where**: `schema_history::applied_at` (1046), `now_text` (1083–1086).

**Problem**: local wall-clock text without zone; unsortable across hosts.

**Fix**: store ISO 8601 UTC with `Z` (`std::chrono::utc_clock` or
`system_clock` formatted `{:%FT%TZ}`); add first-class
`std::chrono::sys_time<…>` column support (INTEGER epoch seconds or
milliseconds, dialect-independent) so applications stop storing
timestamps as JSON text.

**Test**: round-trip `sys_seconds`; `applied_at` matches
`^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$`.

---

## S-12 · P2 · Driver conformance suite and CI for non-SQLite drivers

**Where**: `tests/`, `CMakeLists.txt`.

**Problem**: only SQLite is exercised; `pg.hpp` and `mariadb.hpp` compile
"where headers exist" and are untested; any third-party driver (such as
storax-ca's wire-protocol Postgres client) has no contract to test
against.

**Fix**:
- `tests/conformance.hpp`: a header-only suite parameterised on a
  `salt::db` factory: types round-trip (all `col_kind`s, NULL, empty
  string vs NULL, empty blob, max/min ints, NaN/inf policy), `RETURNING`
  vs `last_insert_id`, multi-statement `exec`, prepare rejects multiple
  statements, transaction rollback on error, savepoints, nested-unit
  isolation (S-4), `transactional_ddl` capability behaviour (S-3),
  session defaults applied on connect, unicode and NUL-containing text,
  1 MiB blob, 10 000-row fetch.
- CI job with a Postgres service container that builds `pg.hpp` and
  runs the conformance suite; same for MariaDB if it stays supported.
- storax-ca links the same conformance header against its own driver.

---

## S-13 · P2 · `count()` and `query()` tail handling consistency

**Where**: `db::count` (883–893) appends the raw tail then relies on
`scalar()` to adapt placeholders across the whole statement.

**Problem**: works today, but the two paths differ; after S-6 the
tokenizer must only see the tail (the generated prefix contains no
user text, but `SELECT COUNT(*) FROM "t"` contains double quotes, which
the tokenizer would then have to handle).

**Fix**: adapt the tail before concatenation everywhere; the generated
prefix is never passed through the rewriter.

**Test**: `count<User>("WHERE name = ?", "x")` on `postgres_dialect`
yields `… WHERE name = $1`.

---

## S-14 · P2 · Documentation additions

- **Threading**: `db` is single-connection and not thread-safe; state
  that a pool is the application's job and give the recommended pattern
  (one `db` per worker, never shared across threads).
- **SQL injection contract**: tails, `check`, `sql_default`, and
  migration SQL are trusted developer text; values must always be binds;
  after S-1, "trusted" is enforced by the type system.
- **Security model of migrations**: what the checksum proves and does
  not prove (after S-3).
- **Dialect capability table**: `transactional_ddl`, `returning`,
  savepoint support, implicit-commit statements (MySQL's list is long:
  DDL, `LOCK TABLES`, `START TRANSACTION`, administrative statements),
  default isolation, and which errors map to `errc::retryable`.
- **SQLite deployment guidance** (after S-7): vendored amalgamation,
  compile options, file permissions (0600, directory 0700), WAL file
  handling in containers, backup via `VACUUM INTO` not file copy.

---

## S-15 · P2 · Oracle support

**DONE (2026-09-15).** `oracle_dialect` (`:n` placeholders, NUMBER/
BINARY_DOUBLE/VARCHAR2(4000)/BLOB, identity pk, quoted-UPPERCASE
identifiers via `dialect.fold_upper`), `oracle.hpp` OCI driver
(RETURNING ... INTO out-bind for ids, temporary-LOB blob binds so empty
blob ≠ NULL, client-side ';' splitter + PL/SQL passthrough, transaction
statement recognition, AL32UTF8 env), gvenzl/oracle-free:23-slim in the
compose file, `tests/test_oracle.cpp` green: conformance (with the
documented '' IS NULL carve-out) + returning/exec-shapes/number-shaping/
isolation/migration-caveat/tamper/lock-conflict suites.

- add support for Oracle DB
- use gvenzl/oracle-free container

## Mapping to storax-ca

| Note | storax-ca test id / plan ref |
|---|---|
| S-1 | T-build-4 (negative compile), T-authz-2 (tenant isolation) |
| S-2 | T-iss-1 golden fixtures (serial storage), enterprise plan §0.1 |
| S-3 | T-k8s-5 (migration safety), T-aud-3 (tamper detection) |
| S-4 | T-wf-3 (vote race), T-dos-6 (torn issuance), enterprise plan §0.1 dispatcher-transaction invariant |
| S-5 | T-authz-3 spirit — no silent acceptance |
| S-7 | T-sep-6 container hardening, §12 secrets (file permissions) |
| S-8 | T-aud-2 redaction |
| S-9, S-12 | Postgres driver suite (security plan §2, §7 T-fuzz-5) |