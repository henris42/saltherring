# saltherring — Improvement Notes

Source: read-only review of `henris42/saltherring` at commit `9262310`
(`include/saltherring/*.hpp`, `tests/test_saltherring.cpp`). Not built
(no GCC 16 in the review environment). Line numbers refer to that commit.

Audience: coding agent or engineer. Each note has an id, priority, the
location, the problem, the fix, and the test that proves it. Work in id
order; S-1 changes the public API and should land first so the others
build on it.

Priorities: **P0** — breaks a storax-ca invariant or corrupts CA data;
**P1** — correctness under CA load; **P2** — hardening and hygiene.

## Status (2026-09-15)

Implemented and tested (headers + `tests/test_saltherring.cpp`, `tests/nc/`,
`tests/conformance.hpp`; suite green under GCC 16.1):
S-1, S-2 (option 1: compile-time rejection), S-3, S-4, S-5, S-6, S-7, S-8,
S-9 (pg.hpp side; conformance suite exists per S-12), S-10, S-11, S-13,
S-14 (README security model + NOTES.md updates).

Remaining: S-12's CI job with Postgres/MariaDB service containers (no CI
config in this repo yet; `tests/conformance.hpp` is ready to point at those
drivers), and S-9's driver checks are unexercised on this machine (no
libpq-dev). Bonus fix found by the conformance suite: the SQLite driver
bound empty blobs as NULL (`sqlite3_bind_blob` with a null `data()`).

---

## S-1 · P0 · Compile-time-only SQL tails (tenant-scope support)

**Where**: `saltherring.hpp` — `db::exec` (744), `db::scalar` (758),
`db::query` (831), `db::query_one` (844), `db::count` (883).

**Problem**: All take a runtime `std::string_view` tail. Nothing prevents
a caller from formatting user input into SQL. storax-ca's invariant
"cross-tenant reads impossible by construction" needs the persistence
layer to make runtime-built SQL uncompilable so a tenant-scoped wrapper
can be the only caller.

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
- Provide `unchecked_sql` overloads so existing callers can migrate
  explicitly; storax-ca's lint bans `unchecked_sql` outside its
  tenant-scope module.

**Test**: negative-compile file `tests/nc/runtime_sql.cpp` —
`db.query<User>(std::string("WHERE ") + x)` must fail; positive test that
literal tails still work. storax-ca T-build-4 references this.

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
- Add `migrate_options::allow_unwind_missing_in_prod = false` (or make
  `unwind_missing` require an explicit token type) so a production
  binary cannot unwind history it does not know unless the operator
  opts in per call.
- Document: a cryptographic tamper-evidence layer (signed history) is
  the application's job; storax-ca must not reuse this checksum for its
  audit chain.

**Test**: apply v1–v2, edit `down_sql` of v2 directly via `raw()`, call
`rollback(db, 1)` → `migration_tampered`, table unchanged; same for
`unwind_missing`.

---

## S-4 · P1 · Exception-safe transactions, savepoints, isolation

**Where**: `db::transaction` (897–906).

**Problem**: if `f()` throws, no `ROLLBACK` runs; the connection stays
inside a transaction and the next `BEGIN` fails. No nesting, no way to
choose isolation.

**Fix**:
```cpp
template <typename F>
result<void> transaction(F&& f, tx_options o = {}) {
  if (auto b = exec(begin_sql(o)); !b) return b;   // BEGIN [ISOLATION LEVEL …]
  bool done = false;
  struct guard { db* d; bool* done; ~guard(){ if(!*done) (void)d->exec("ROLLBACK"); } } g{this,&done};
  result<void> r = std::forward<F>(f)();
  if (!r) return r;                                 // guard rolls back
  r = exec("COMMIT"); done = true; return r;
}
```
- `tx_options{ isolation: read_committed|repeatable_read|serializable, read_only }`
  rendered per dialect (SQLite: `BEGIN IMMEDIATE` for write intent).
- Nested calls use `SAVEPOINT sp_<depth>` / `RELEASE` / `ROLLBACK TO`;
  track depth in `db`.
- Provide `for_update` helper or document `SELECT … FOR UPDATE` in tails
  (Postgres) and `BEGIN IMMEDIATE` (SQLite) for read-modify-write
  patterns such as storax-ca's quorum computation.

**Test**: `transaction([]{ throw std::runtime_error{}; })` leaves the db
usable (next `BEGIN` succeeds, no partial rows); nested savepoint rolls
back inner only; SQLite `BEGIN IMMEDIATE` blocks a second writer.

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
  `PRAGMA journal_mode = WAL` (option), `PRAGMA synchronous = FULL`
  (option, default for CA use), `PRAGMA secure_delete = ON` (option,
  default on), `PRAGMA trusted_schema = OFF`, keep
  `PRAGMA foreign_keys = ON`.
- Add the missing declarations to the ABI shim (`sqlite3_open_v2`,
  `sqlite3_db_config`, `sqlite3_busy_timeout`,
  `sqlite3_extended_result_codes`, `sqlite3_libversion_number`) and
  refuse to open below a minimum `sqlite3_libversion_number()` (pick a
  version and document why).
- Note for storax-ca: vendor the amalgamation compiled with
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
  is left empty and the error notes "(redacted)".
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
  statements, transaction rollback on error, savepoints, unicode and
  NUL-containing text, 1 MiB blob, 10 000-row fetch.
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
- **SQLite deployment guidance** (after S-7): vendored amalgamation,
  compile options, file permissions (0600, directory 0700), WAL file
  handling in containers, backup via `VACUUM INTO` not file copy.

---

## Mapping to storax-ca

| Note | storax-ca test id / plan ref |
|---|---|
| S-1 | T-build-4 (negative compile), T-authz-2 (tenant isolation) |
| S-2 | T-iss-1 golden fixtures (serial storage), enterprise plan §0.1 |
| S-3 | T-k8s-5 (migration safety), T-aud-3 (tamper detection) |
| S-4 | T-wf-3 (vote race), T-dos-6 (torn issuance) |
| S-5 | T-authz-3 spirit — no silent acceptance |
| S-7 | T-sep-6 container hardening, §12 secrets (file permissions) |
| S-8 | T-aud-2 redaction |
| S-9, S-12 | Postgres driver suite (security plan §2, §7 T-fuzz-5) |
