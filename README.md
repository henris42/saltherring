# saltherring

SQL persistence for C++26: tables, typed queries and schema version control
for any struct, out of the box. Reflection maps objects to rows, annotations
declare the schema, contracts guard the API — no macros, no codegen, no
interface to implement. Column encoding is shared with
[sardine](https://github.com/henris42/sardine): what sardine can serialize, saltherring can persist.

```cpp
struct [[=salt::table("users")]] User {
  [[=salt::auto_pk]]  std::int64_t id = 0;   // DB-assigned, written back
  [[=salt::unique{}]] std::string email;
  std::string name;
  [[=salt::check("balance >= 0")]] double balance = 0;
  std::optional<std::string> nickname;       // nullable column
  Address addr;                              // any sardine type → JSON TEXT
  [[=salt::transient{}]] int cache = -1;     // not persisted
};

salt::db db = *salt::sqlite::open("app.db");
db.create_table<User>();                     // CREATE TABLE IF NOT EXISTS ...

User u{.email = "henri@example.com", .name = "Henri", .balance = 12.5};
db.insert(u);                                // u.id is now the row id
std::expected<std::optional<User>, salt::error> back = db.find<User>(u.id);
auto rich = db.query<User>("WHERE balance > ? ORDER BY name", 10.0);
db.update(u);
db.erase<User>(u.id);
```

Everything returns `std::expected` with a coded `salt::error`, sardine-style.
Enums store their sardine wire name (`rename_all` honored), `uint8_t`
sequences store as BLOBs, and any other sardine-serializable member —
nested structs, vectors, maps, variants — rides in a TEXT column as sardine
JSON.

## Schema version control

Flyway-shaped, with the changelog proven on every run and unwindable:

```cpp
constexpr salt::migration schema[] = {
    {.version = 1, .description = "create users",
     .sql  = "CREATE TABLE users (...)",
     .down = "DROP TABLE users"},
    {.version = 2, .description = "add nickname",
     .sql  = "ALTER TABLE users ADD COLUMN nickname TEXT",
     .down = "ALTER TABLE users DROP COLUMN nickname"},
};

salt::migrate(db, schema);      // applies what's pending, verifies the rest
salt::validate(db, schema);     // the same proof, applying nothing
salt::rollback(db, 1);          // unwind back to version 1, newest first
```

Applied migrations land in `salt_schema_history` with a checksum over
version‖description‖up‖down and a verbatim copy of the down SQL. Editing an
applied migration is an error; a history row that no longer matches its own
checksum is refused (`errc::migration_tampered`) before its stored down SQL
would run. The recorded down SQL is what unwinds, so a new deployment can
roll back changelog entries whose code it no longer carries:
`salt::migrate(db, schema, {.unwind_missing = true})`.

## Backends

| driver | header | status |
|---|---|---|
| SQLite | `<saltherring/sqlite.hpp>` | tested; needs only the shared library |
| PostgreSQL | `<saltherring/pg.hpp>` | dialect tested; driver compiles where libpq-dev exists |
| MariaDB/MySQL | `<saltherring/mariadb.hpp>` | dialect tested; driver compiles where client headers exist |

SQL generation is a pure function of (reflected model, dialect), so the
Postgres and MariaDB texts — `$1` placeholders, `BIGSERIAL`/`AUTO_INCREMENT`,
`RETURNING`, backtick quoting, `VARCHAR` keys — are unit-tested without a
server.

## Security model

**SQL text is compile-time by construction.** Statement tails for
`exec`/`scalar`/`query`/`query_one`/`count` are `salt::sql`, whose only
constructor is `consteval` — formatting user input into SQL does not
compile (proven by the negative-compile tests in `tests/nc/`). Values
always travel as binds. The one escape hatch for genuinely
runtime-assembled SQL is `salt::unchecked_sql`, deliberately ugly and
greppable; ban it outside one reviewed module and injection is
unrepresentable. Identifier annotations (`table`, `column`, `references`)
admit only `[A-Za-z_][A-Za-z0-9_]*` (≤ 63 chars), enforced at compile
time. `check`/`sql_default` expressions and migration SQL are trusted
developer text.

**Errors keep values out of logs.** `error.message` never embeds stored
values; the offending value rides in `error.detail` (capped at 512 bytes),
and `[[=salt::sensitive{}]]` members clear even that. `error.sql` holds
statement text only — bound values never appear.

**What the migration checksum proves — and doesn't.** FNV-1a over
version‖description‖up‖down catches drift in both directions: an edited
migration no longer matches its row, an edited row no longer matches its
own checksum and its down SQL is refused. It is *not* tamper evidence
against an attacker who can write `salt_schema_history` and recompute the
hash — cryptographic history (signing, WORM audit) is the application's
layer.

**Threading.** A `salt::db` is one connection and is not thread-safe.
Pooling is the application's job; the pattern is one `db` per worker,
never shared across threads.

**SQLite deployment.** `salt::sqlite::open()` is hardened by default —
NOFOLLOW, URI filenames off, defensive mode, extension loading off,
`trusted_schema` OFF, `secure_delete` ON, `synchronous` FULL, busy
timeout, minimum library version 3.35 — each with an off switch in
`salt::sqlite::options`. For CA-grade deployments vendor the amalgamation
(`SQLITE_SECURE_DELETE`, `SQLITE_OMIT_LOAD_EXTENSION`, `SQLITE_DQS=0`)
instead of trusting the container's shared library; keep the file 0600 in
a 0700 directory and back up with `VACUUM INTO`, not file copy.

## Driver conformance

`tests/conformance.hpp` is the `backend::connection` contract as
executable checks — storage-class round-trips at their edges, NULL vs
empty, single-statement `prepare`, multi-statement `exec`, transaction
rollback, savepoints. The SQLite driver passes it in CI; point it at any
`salt::db` factory to certify another driver.

## Building

GCC 16.1, `-std=c++26 -freflection -fcontracts`, sardine checked out as a
sibling (or set `SALTHERRING_SARDINE_DIR`):

```
cmake --preset gcc16 && cmake --build --preset gcc16 && ctest --preset gcc16
```

- [NOTES.md](NOTES.md) — Java-library parity, design decisions, GCC 16.1 quirks
