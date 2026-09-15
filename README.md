# saltherring

ORM and schema migrations for C++26. Reflection maps structs to rows,
annotations declare the schema. No macros, no code generation, no base
classes. Column values are encoded the same way as
[sardine](https://github.com/henris42/sardine), so anything sardine can
serialize can be stored in a column.

```cpp
struct [[=salt::table("users")]] User {
  [[=salt::auto_pk]]  std::int64_t id = 0;   // assigned by the DB on insert
  [[=salt::unique{}]] std::string email;
  std::string name;
  [[=salt::check("balance >= 0")]] double balance = 0;
  std::optional<std::string> nickname;       // nullable column
  Address addr;                              // any sardine type, stored as JSON
};

salt::db db = *salt::sqlite::open("app.db");
db.create_table<User>();

User u{.email = "henri@example.com", .name = "Henri", .balance = 12.5};
db.insert(u);                                // u.id is now set
auto rich = db.query<User>("WHERE balance > ? ORDER BY name", 10.0);
db.update(u);
db.erase<User>(u.id);
```

Every call returns `std::expected<T, salt::error>`.

Statement text must be a compile-time literal; building SQL from strings
at runtime does not compile unless you write `salt::unchecked_sql`
explicitly. Values always travel as bound parameters.

## Migrations

```cpp
constexpr salt::migration schema[] = {
    {.version = 1, .description = "create users",
     .sql  = "CREATE TABLE users (...)",
     .down = "DROP TABLE users"},
    {.version = 2, .description = "add nickname",
     .sql  = "ALTER TABLE users ADD COLUMN nickname TEXT",
     .down = "ALTER TABLE users DROP COLUMN nickname"},
};

salt::migrate(db, schema);      // apply what's pending, verify the rest
salt::rollback(db, 1);          // unwind back to version 1
```

Works like Flyway: applied migrations are recorded in a history table
with a checksum, and editing an already-applied migration is an error.
The down SQL is recorded too, so rollback works even from a binary that
no longer contains the migration. History rows that fail their checksum
are refused before their down SQL would run.

## Backends

| driver | header | needs |
|---|---|---|
| SQLite | `<saltherring/sqlite.hpp>` | just `libsqlite3.so.0` (3.37+) |
| PostgreSQL | `<saltherring/pg.hpp>` | libpq-dev |
| MariaDB/MySQL | `<saltherring/mariadb.hpp>` | libmariadb-dev |
| Oracle 23ai+ | `<saltherring/oracle.hpp>` | Instant Client SDK |

All four pass the same conformance suite, tested against real servers in
containers (`scripts/server-tests.sh`). Without Docker the server tests
skip and the rest of `ctest` runs normally. Install and setup details per
backend are in
[user-guide.md](user-guide.md#backend-setup-what-each-one-needs-on-the-host).

Worth knowing: error messages never include stored values, SQLite files
open with hardened defaults, and a `salt::db` is one connection and not
thread-safe (use one per thread). Details in the
[user guide](user-guide.md).

## Building

GCC 16.1 with `-std=c++26 -freflection -fcontracts`, sardine checked out
as a sibling (or set `SALTHERRING_SARDINE_DIR`):

```
cmake --preset gcc16 && cmake --build --preset gcc16 && ctest --preset gcc16
```

- [user-guide.md](user-guide.md) — the full API
- [examples/](examples/) — runnable examples
- [NOTES.md](NOTES.md) — design decisions, GCC 16.1 quirks
