# saltherring user guide

SQL persistence for C++26: tables, typed queries and schema version control
for any struct. Reflection maps objects to rows, annotations declare the
schema, contracts guard the API — no macros, no codegen, no interface to
implement. Column encoding is shared with [sardine](https://github.com/henris42/sardine):
what sardine can serialize, saltherring can persist.

This guide covers the whole public API. Runnable versions of most snippets
live in [examples/](examples/) (`quickstart`, `columns`, `transactions`,
`migrations` — built as `example_<name>` targets). For design rationale see
[NOTES.md](NOTES.md); for the security model in one place see the
[README](README.md#security-model).

- [Requirements and building](#requirements-and-building)
- [Five-minute tour](#five-minute-tour)
- [Modeling: structs to tables](#modeling-structs-to-tables)
- [Opening a database](#opening-a-database)
- [Backend setup: what each one needs on the host](#backend-setup-what-each-one-needs-on-the-host)
- [Reading and writing](#reading-and-writing)
- [SQL text: the rules](#sql-text-the-rules)
- [Errors](#errors)
- [Transactions](#transactions)
- [Migrations](#migrations)
- [Dialects and drivers](#dialects-and-drivers)
- [Threading](#threading)

## Requirements and building

GCC 16.1+ with `-std=c++26 -freflection -fcontracts`, linked against
`stdc++exp` (the contract-violation handler lives there), and a sardine
checkout on the include path. Header-only: include `<saltherring/saltherring.hpp>`
plus one driver header.

With CMake, link the interface target and everything is set up:

```cmake
add_subdirectory(saltherring)   # or FetchContent
target_link_libraries(app PRIVATE saltherring::saltherring)
# sardine is expected as a sibling checkout; override with
# -DSALTHERRING_SARDINE_DIR=/path/to/sardine
```

In this repository: `cmake --preset gcc16 && cmake --build build && ctest --test-dir build`.

The SQLite driver needs only the shared library (`-lsqlite3`, or
`-l:libsqlite3.so.0` without the dev package) — it declares the frozen C ABI
itself when `<sqlite3.h>` is absent.

## Five-minute tour

```cpp
#include <saltherring/saltherring.hpp>
#include <saltherring/sqlite.hpp>

struct [[=salt::table("users")]] User {
  [[=salt::auto_pk]]  std::int64_t id = 0;   // DB-assigned, written back
  [[=salt::unique{}]] std::string email;
  std::string name;
  [[=salt::check("balance >= 0")]] double balance = 0;
  std::optional<std::string> nickname;       // nullable column
};

int main() {
  salt::db db = *salt::sqlite::open("app.db");
  db.create_table<User>();                   // CREATE TABLE IF NOT EXISTS ...

  User u{.email = "henri@example.com", .name = "Henri", .balance = 12.5};
  db.insert(u);                              // u.id now holds the row id

  auto rich = db.query<User>("WHERE balance > ? ORDER BY name", 10.0);
  for (const User& r : *rich) { /* ... */ }

  u.balance = 99;
  db.update(u);
  db.erase<User>(u.id);
}
```

Every operation returns `std::expected<T, salt::error>` — check it, or
dereference when failure is a bug.

## Modeling: structs to tables

Any aggregate with at least one persisted member is an entity. Annotations
refine the mapping; everything has a sensible default.

| annotation | on | meaning |
|---|---|---|
| `[[=salt::table("users")]]` | struct | table name; default: type name PascalCase → snake_case (`AccountEntry` → `account_entry`) |
| `[[=salt::column("created")]]` | member | column name; default: the member identifier |
| `[[=salt::pk{}]]` | member | primary key — exactly one per struct for the by-id operations |
| `[[=salt::auto_pk]]` | member | DB-assigned primary key (AUTOINCREMENT / BIGSERIAL / AUTO_INCREMENT); must be a non-optional integer; `insert()` skips it and writes the assigned id back |
| `[[=salt::transient{}]]` | member | not persisted — invisible to DDL, reads and writes |
| `[[=salt::unique{}]]` | member | UNIQUE constraint |
| `[[=salt::indexed{}]]` | member | `CREATE INDEX idx_<table>_<column>` alongside the table |
| `[[=salt::check("balance >= 0")]]` | member | CHECK constraint, expression verbatim |
| `[[=salt::sql_default("0")]]` | member | DEFAULT clause, expression verbatim (quote string literals yourself: `sql_default("'pending'")`) |
| `[[=salt::references("users")]]` / `("users", "id")` | member | FOREIGN KEY; column defaults to `"id"` |
| `[[=salt::sensitive{}]]` | member | decode errors for this member never carry the stored value (see [Errors](#errors)) |

GCC 16.1 quirk: `[[=salt::column{"x"}]]` does not parse — use the call
syntax, `[[=salt::column("x")]]`.

Table and column names are validated at compile time:
`[A-Za-z_][A-Za-z0-9_]*`, at most 63 characters. A quote, space or empty
name is a compile error, which is why the generated SQL never needs to
escape identifiers.

### Column types

The member type picks the SQL shape. `std::optional<T>` makes any of them
nullable (`NULL` ↔ `nullopt`); non-optional members are `NOT NULL`.

| member type | column | stored as |
|---|---|---|
| `bool` | BOOLEAN / INTEGER | 0 / 1 |
| signed integers ≤ 64 bit, unsigned < 64 bit | INTEGER / BIGINT | value; reads that don't fit the member are `errc::out_of_range` |
| `float`, `double` | REAL / DOUBLE PRECISION | value |
| `std::string` and string-likes | TEXT | value (NUL bytes and unicode round-trip) |
| sequences of `std::uint8_t` | BLOB | bytes, exactly where sardine's CBOR would emit a byte string |
| enums | TEXT | the sardine wire name (`rename` / `rename_all` honored); `sardine::enum_from_number` additionally lets INTEGER or numeric text decode |
| `std::chrono::sys_time<D>` | INTEGER | epoch tick count in the member's own duration — `sys_seconds` stores epoch seconds; sortable and dialect-independent |
| anything else sardine serializes | TEXT | sardine JSON (structs, vectors, maps, variants...) |

**64-bit unsigned integers do not compile.** A `std::uint64_t` above
`INT64_MAX` would store negative in a SQL BIGINT, so the mapping is rejected
with a `static_assert` — store such values as text or a BLOB, deliberately.
(`tests/nc/uint64_column.cpp` proves the rejection.)

Introspection, mostly for tests: `salt::columns_of<T>()` returns the
reflected `column_info` span, `salt::table_of<T>()` the table name, and
`salt::create_table_sql<T>(dialect)` the DDL text for any dialect — a pure
function, no connection needed.

## Opening a database

### SQLite

```cpp
auto db = salt::sqlite::open("app.db");          // hardened defaults
auto mem = salt::sqlite::open_memory();          // ":memory:" scratch db
auto ro  = salt::sqlite::open("app.db", {.read_only = true});
```

`open()` is hardened by default; every default has an off switch in
`salt::sqlite::options`:

| option | default | effect |
|---|---|---|
| `create` | `true` | create the file if missing; `false` makes a missing file an error |
| `read_only` | `false` | open read-only |
| `follow_symlinks` | `false` | refuse symlinked database paths (NOFOLLOW) |
| `uri` | `false` | allow `file:` URI filenames |
| `wal` | `false` | `PRAGMA journal_mode = WAL` |
| `full_sync` | `true` | `PRAGMA synchronous = FULL` |
| `secure_delete` | `true` | `PRAGMA secure_delete = ON` |
| `busy_timeout_ms` | `5000` | `sqlite3_busy_timeout` |

Always on: extended result codes, defensive mode, extension loading
disabled, `PRAGMA foreign_keys = ON`, `PRAGMA trusted_schema = OFF`, and a
minimum library version of 3.37.0 (older libraries fail `open()` rather
than failing mysteriously mid-migration).

Deployment tips for sensitive data: consider vendoring the SQLite
amalgamation compiled with `SQLITE_SECURE_DELETE`,
`SQLITE_OMIT_LOAD_EXTENSION` and `SQLITE_DQS=0` instead of trusting
whatever shared library the container image ships; keep the database file
0600 in a 0700 directory; back up with `VACUUM INTO`, not by copying the
file (a copy taken mid-write is corrupt).

### PostgreSQL and MariaDB

```cpp
#include <saltherring/pg.hpp>       // needs libpq-dev
auto db = salt::pg::open("host=localhost dbname=app user=app");

#include <saltherring/mariadb.hpp>  // needs the MariaDB/MySQL client headers
auto db2 = salt::mariadb::open("localhost", "app", password, "appdb");  // port = 3306

#include <saltherring/oracle.hpp>   // needs the Oracle Instant Client SDK
auto db3 = salt::oracle::open("localhost:1521/FREEPDB1", "app", password);
```

The server drivers compile only where their client headers exist
(`libpq-dev` / `libmariadb-dev` / the Instant Client SDK via
`-DSALTHERRING_ORACLE_CLIENT_DIR`), and all pass the
[conformance suite](#dialects-and-drivers) plus dialect-specific suites
against real containerized servers: `scripts/server-tests.sh` starts
PostgreSQL, MariaDB and Oracle Free via Docker Compose, runs
`ctest -L server`, and tears them down. Without a reachable server those
tests skip, so plain `ctest` needs no Docker.

Engine notes the drivers surface rather than hide:

- **MariaDB** — text/blob columns are `LONGTEXT`/`LONGBLOB` (plain
  `TEXT`/`BLOB` cap at 64 KiB).
- **Postgres** — TEXT cannot store NUL bytes; the driver refuses such
  binds with `errc::bind` rather than truncating.
- **Oracle (23ai+)** — `''` IS NULL: an empty optional string reads back
  as `nullopt`, and a non-optional empty string is a NOT NULL violation.
  Text columns are `VARCHAR2(4000)`; store larger text as a BLOB.
  Identifiers are quoted UPPERCASE so unquoted names in tails keep
  working; ids arrive via `RETURNING ... INTO`; DDL commits implicitly
  (the MariaDB migration caveat applies).

## Backend setup: what each one needs on the host

saltherring is header-only; what varies per backend is which client pieces
must exist on the machine that builds and runs your application. The
servers themselves can live anywhere (the test suites run them in
containers) — only the client library is a host concern.

| backend | build needs | runtime needs | CMake |
|---|---|---|---|
| SQLite | nothing (header optional) | `libsqlite3.so.0` ≥ 3.37 | link `-lsqlite3` or `-l:libsqlite3.so.0` |
| PostgreSQL | `libpq-dev` | `libpq.so.5` (`libpq5`) | auto-detected |
| MariaDB/MySQL | `libmariadb-dev` | `libmariadb.so.3` | auto-detected |
| Oracle | Instant Client **sdk** zip | Instant Client **basiclite** zip (+ libaio, see below) | `-DSALTHERRING_ORACLE_CLIENT_DIR` |

### SQLite

Nothing to install for the build: when `<sqlite3.h>` is absent the driver
declares the frozen C ABI itself. At runtime only the shared library must
resolve — link `-lsqlite3` with the dev package, or `-l:libsqlite3.so.0`
without it. The library must be 3.37.0+ (`sqlite3_changes64`,
`RETURNING`); older libraries fail `open()` with a clear message rather
than misbehaving later.

### PostgreSQL

```
apt install libpq-dev          # build: postgresql/libpq-fe.h
```

Runtime needs `libpq.so.5` (package `libpq5`, pulled in by `libpq-dev`);
deployment hosts that only run the binary need just `libpq5`. This
repository's CMake finds both automatically and builds `test_pg` when
present.

### MariaDB/MySQL

```
apt install libmariadb-dev     # build: mariadb/mysql.h
```

Runtime needs `libmariadb.so.3` (package `libmariadb3`). Auto-detected by
CMake the same way.

### Oracle

Oracle's client is not in the distro archives — download both Instant
Client zips (free, no login) and unpack them into one directory:

```
mkdir -p /opt/oracle && cd /opt/oracle
curl -LO https://download.oracle.com/otn_software/linux/instantclient/instantclient-basiclite-linuxx64.zip
curl -LO https://download.oracle.com/otn_software/linux/instantclient/instantclient-sdk-linuxx64.zip
unzip instantclient-basiclite-linuxx64.zip && unzip instantclient-sdk-linuxx64.zip
# → /opt/oracle/instantclient_23_26 with libclntsh.so* and sdk/include/oci.h
```

Point the build at it:

```
cmake --preset gcc16 -DSALTHERRING_ORACLE_CLIENT_DIR=/opt/oracle/instantclient_23_26
```

Two things every Oracle-linked binary must get right at runtime:

1. **Library resolution must be transitive.** `libclntsh.so` pulls in
   `libclntshcore`, `libnnz` and `libaio` from its own directory, and the
   modern `DT_RUNPATH` is *not* consulted for a library's own
   dependencies. Either export the directory —
   `export LD_LIBRARY_PATH=/opt/oracle/instantclient_23_26` — or link
   with classic RPATH the way this repo's tests do:
   `-Wl,--disable-new-dtags,-rpath,/opt/oracle/instantclient_23_26`.
   (A third option for system-wide installs: drop the path into
   `/etc/ld.so.conf.d/oracle.conf` and run `ldconfig`.)
2. **libaio, with the Ubuntu 24.04 rename.** `libclntsh` needs
   `libaio.so.1`; Ubuntu 24.04's package is `libaio1t64` and its soname is
   `libaio.so.1t64`, so the package alone does not satisfy the loader.
   Create the symlink once, somewhere on the resolution path — the client
   directory itself is the tidy spot:

   ```
   apt install libaio1t64
   ln -s /usr/lib/x86_64-linux-gnu/libaio.so.1t64 \
         /opt/oracle/instantclient_23_26/libaio.so.1
   ```

   (Distros whose package still ships `libaio.so.1` — Debian 12, RHEL —
   need no symlink. The "no version information available" warning the
   symlink produces is benign.)

### The server test suites

`ctest -L server` connects using env vars, with defaults matching
[tests/containers/docker-compose.yml](tests/containers/docker-compose.yml):
`SALT_PG_DSN`, `SALT_MARIADB_HOST/PORT/USER/PASSWORD`,
`SALT_ORACLE_CONNECT`/`SALT_ORACLE_SYSTEM_PASSWORD`. No reachable server
means SKIP, never FAIL. One caveat: after a cold `docker compose down -v`,
Oracle rebuilds its database and registers the `FREEPDB1` service a minute
or so *after* the container reports healthy — a suite run in that window
skips; rerun and it passes.

## Reading and writing

All operations live on `salt::db` (movable, not copyable — one connection).

```cpp
db.create_table<User>();               // CREATE TABLE IF NOT EXISTS + indexes
db.create_tables<User, Event, Order>();// several, in order

User u{.email = "a@x", .name = "A"};
auto id = db.insert(u);                // result<int64>: the primary key.
                                       // auto_pk: DB-assigned id, written
                                       // back into u when u is non-const

auto found = db.find<User>(u.id);      // result<optional<User>> — absence
if (found && *found) { ... }           // is nullopt, not an error

auto all   = db.query<User>();                          // whole table
auto some  = db.query<User>("WHERE balance > ? ORDER BY name", 10.0);
auto one   = db.query_one<User>("WHERE email = ?", email);  // first row, if any
auto n     = db.count<User>("WHERE active = ?", true);      // result<int64>

db.update(u);                          // UPDATE ... WHERE pk = ?
db.erase<User>(u.id);                  // DELETE ... WHERE pk = ?
```

`query`/`query_one`/`count` take a *tail* — everything after the generated
`SELECT columns FROM table` — so the column list and table name always come
from the model. Binds are typed: pass values of the member types (or
anything with the same mapping) and they travel as parameters, never as
text.

Raw statements, when the mapper doesn't fit:

```cpp
db.exec("PRAGMA analysis_limit = 400");                 // no result rows
db.exec("UPDATE users SET active = ? WHERE id = ?", false, id);
auto count = db.scalar<std::int64_t>("SELECT COUNT(*) FROM users");
auto name  = db.scalar<std::string>("SELECT name FROM users WHERE id = ?", id);
```

`scalar<V>` decodes the first column of the first row using the same rules
as members; no row is `errc::no_rows`. `db.raw()` exposes the underlying
`backend::connection` when nothing else will do.

## SQL text: the rules

**Statement text is compile-time by construction.** Every text parameter
above is a `salt::sql`, whose only constructor is `consteval` — a string
literal works, anything runtime does not compile:

```cpp
db.query<User>("WHERE name = ?", name);              // ok: literal tail
std::string tail = "WHERE name = " + name;
db.query<User>(tail);                                // does not compile
db.query<User>(("WHERE name = " + name).c_str());    // does not compile
```

Values always travel as binds. When you genuinely must assemble SQL at
runtime (a report builder, an admin console), announce it:

```cpp
db.query<User>(salt::unchecked_sql{tail}, binds...);
```

`unchecked_sql` is deliberately ugly and greppable — ban it outside one
reviewed module (a lint rule on the identifier is enough) and SQL injection
is unrepresentable in the rest of the codebase.

**Placeholders are `?` everywhere.** Tails and raw statements use `?`; the
library rewrites them to the dialect's style (`$1 $2` on Postgres) with a
real tokenizer: text inside `'strings'` (with `''` doubling), `"quoted"`
and `` `quoted` `` identifiers, `--` line and nested `/* block */`
comments, and Postgres `$tag$...$tag$` strings passes through untouched.
`??` in code position escapes a literal `?` — Postgres JSON operator users
write `data ?? 'key'`. Unbalanced quotes or comments are `errc::prepare`,
not a guess.

**`prepare` is one statement; `exec` is many.** A bound statement
(`exec` with args, `scalar`, `query`...) refuses trailing statements —
`"UPDATE ...; DROP TABLE ..."` is `errc::prepare` and nothing runs.
Multi-statement text is only accepted by `exec` without binds, where it is
explicit and useful (schema setup, pragmas).

## Errors

```cpp
template <typename T> using result = std::expected<T, salt::error>;

struct error {
  std::string message;  // human explanation; never contains stored values
  std::string detail;   // the offending value, when useful; capped at 512;
                        // empty for [[=salt::sensitive{}]] members
  errc code;            // branch on this
  std::string sql;      // statement text, when there is one; capped;
                        // bound values never appear here
};
```

Codes: `closed`, `connect`, `prepare`, `bind`, `exec` (constraint
violations land here), `type_mismatch`, `out_of_range`, `unknown_enum`,
`serialization`, `no_rows`, and the migration family — `migration_duplicate`,
`migration_checksum`, `migration_missing`, `migration_order`,
`migration_no_down`, `migration_tampered`.

The value/message split is a logging contract: `message` is always safe to
log; route `detail` to debug-level or drop it. Mark columns whose values
must never reach logs at all:

```cpp
struct Secret {
  [[=salt::pk{}]] std::int64_t id = 0;
  [[=salt::sensitive{}]] Token token;   // errors say "(value redacted)"
};
```

Decode errors name the column and table (`... (column 'level' of events)`),
so a failed row read is diagnosable without the value.

## Transactions

```cpp
auto r = db.transaction([&]() -> salt::result<void> {
  if (auto i = db.insert(entry); !i) return std::unexpected(i.error());
  if (auto u = db.update(account); !u) return u;
  return {};
});
```

BEGIN, run, COMMIT. If the callable returns an error **or throws**, a guard
issues ROLLBACK and the connection is left outside any transaction, fully
usable — the error propagates in the result, the exception continues
unwinding.

Nested `transaction` calls become savepoints: an inner failure rolls back
only the inner work, the outer transaction decides whether to continue.

Options:

```cpp
db.transaction(f, {.immediate = true});               // SQLite: BEGIN IMMEDIATE
db.transaction(f, {.iso = salt::isolation::serializable});   // Postgres/MariaDB
db.transaction(f, {.iso = salt::isolation::repeatable_read, .read_only = true});
```

SQLite is serializable by construction and ignores `iso`/`read_only`, but
honors `immediate` — declare write intent up front in read-modify-write
patterns so the write lock is taken before the read, not after.

## Migrations

Flyway-shaped version control for the schema, with the changelog proven on
every run and unwindable from history alone.

```cpp
constexpr salt::migration schema[] = {
    {.version = 1, .description = "create users",
     .sql  = "CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT NOT NULL)",
     .down = "DROP TABLE users"},
    {.version = 2, .description = "add nickname",
     .sql  = "ALTER TABLE users ADD COLUMN nickname TEXT",
     .down = "ALTER TABLE users DROP COLUMN nickname"},
    {.version = 3, .description = "seed defaults",
     .fn   = [](salt::db& d) -> salt::result<void> {   // code instead of SQL
       return d.exec("INSERT INTO users (name) VALUES ('admin')");
     }},
};

auto report = salt::migrate(db, schema);
// report->applied / validated / unwound

salt::validate(db, schema);   // the same proof, applying nothing
salt::rollback(db, 1);        // unwind everything newer than version 1
```

Each pending migration runs in its own transaction (MariaDB DDL
self-commits — same caveat as the Java tools) and lands in
`salt_schema_history` with a checksum over version‖description‖up‖down and
a verbatim copy of the down SQL. On every later run:

- an applied migration whose SQL changed in code → `errc::migration_checksum`
- a pending version older than an applied one → `errc::migration_order`
- two migrations sharing a version → `errc::migration_duplicate`
- history holding a version the list lost → `errc::migration_missing`,
  unless you opt in: `salt::migrate(db, schema, {.unwind_missing = true})`
  rolls the unknown versions back (newest first) with the down SQL recorded
  at apply time — a new deployment can unwind a changelog whose code it
  never carried.

**Tamper detection.** Before any stored down SQL executes — via
`rollback()` or `unwind_missing` — the history row is re-verified against
its own checksum; a mismatch or an unknown checksum rule is
`errc::migration_tampered` and nothing runs. This makes write access to
the history table fail loudly instead of becoming silent SQL execution.
FNV-1a is drift detection, not tamper evidence: an attacker who can write
the table can recompute it. Cryptographic history (signing, WORM audit) is
the application's layer.

Rollback requires recorded down SQL; a migration without `.down` is
`errc::migration_no_down` when unwinding reaches it. Code migrations
(`fn`) checksum a `<code>` marker, so their bodies can be refactored
freely — bump the version if behavior changes.

The history table is a reflected struct like any other — query it:

```cpp
auto hist = db.query<salt::schema_history>("ORDER BY version");
// version, description, checksum, checksum_rule, applied_at (ISO 8601 UTC,
// e.g. "2026-09-15T12:00:00Z"), up_sql, down_sql
```

## Dialects and drivers

A dialect is data — placeholder style, quoting, type names, RETURNING
support — and SQL generation is a pure function of (model, dialect):
`salt::sqlite_dialect`, `salt::postgres_dialect`, `salt::mariadb_dialect`.
That is why the Postgres and MariaDB SQL texts are unit-tested with no
server in sight.

A driver implements two small interfaces over the five shared storage
shapes (`sql_null`, `int64`, `double`, `string`, byte vector):

```cpp
struct backend::statement {
  result<void>      bind(int index, const sql_value&);  // 1-based
  result<bool>      step();                             // true = row available
  result<sql_value> column(int index);                  // 0-based
  int               column_count();
};
struct backend::connection {
  result<std::unique_ptr<backend::statement>> prepare(std::string_view);
  result<void>         exec(std::string_view);          // multi-statement OK
  result<std::int64_t> last_insert_id();
  const dialect&       dial() const;
};
```

The contract is executable: `tests/conformance.hpp` checks storage-class
round-trips at their edges (min/max integers, unicode + NUL text, empty
string vs NULL, empty blob, a 1 MiB blob, 10 000-row fetches),
single-statement `prepare`, multi-statement `exec`, transaction rollback
and savepoints. Point it at any factory:

```cpp
#include "conformance.hpp"
int failures = salt::conformance::run([] {
  return *salt::pg::open("host=ci dbname=conformance");
});
```

The SQLite driver passes it in this repo's test suite; run it against your
own driver (or the bundled pg/mariadb ones on a real server) before
trusting them.

## Threading

A `salt::db` is one connection and is **not thread-safe** — no internal
locking, and transaction state (savepoint depth) lives on the object.
Pooling is the application's job. The pattern that works: one `db` per
worker thread, created where the thread starts, never shared. For SQLite
under concurrent writers, open with `{.wal = true}` and rely on the busy
timeout; declare write intent with `{.immediate = true}` transactions.
