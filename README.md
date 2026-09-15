# saltherring

SQL persistence for C++26: tables, typed queries and schema version control
for any struct, out of the box. Reflection maps objects to rows, annotations
declare the schema, contracts guard the API — no macros, no codegen, no
interface to implement. Column encoding is shared with
[sardine](../sardine): what sardine can serialize, saltherring can persist.

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

Applied migrations land in `salt_schema_history` with a checksum of the up
SQL and a verbatim copy of the down SQL. Editing an applied migration is an
error; the down SQL recorded in the database is what unwinds, so a new
deployment can roll back changelog entries whose code it no longer carries:
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

## Building

GCC 16.1, `-std=c++26 -freflection -fcontracts`, sardine checked out as a
sibling (or set `SALTHERRING_SARDINE_DIR`):

```
cmake --preset gcc16 && cmake --build --preset gcc16 && ctest --preset gcc16
```

- [NOTES.md](NOTES.md) — Java-library parity, design decisions, GCC 16.1 quirks
