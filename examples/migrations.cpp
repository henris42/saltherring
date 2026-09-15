// Migrations: a proven changelog with recorded down SQL — validate on every
// run, roll back on demand, unwind versions a new deployment no longer has.
// Build:  cmake --build build --target example_migrations

#include <saltherring/saltherring.hpp>
#include <saltherring/sqlite.hpp>

#include <cstdlib>
#include <print>

template <typename T>
T need(salt::result<T> r) {
  if (!r) {
    std::println(stderr, "error: {}", r.error().message);
    std::exit(1);
  }
  if constexpr (!std::is_void_v<T>) return std::move(*r);
}

// The changelog: append-only, versions strictly increasing. Record down SQL
// and rollback stays possible from history alone.
constexpr salt::migration kSchema[] = {
    {.version = 1, .description = "create notes",
     .sql  = "CREATE TABLE notes (id INTEGER PRIMARY KEY, body TEXT NOT NULL)",
     .down = "DROP TABLE notes"},
    {.version = 2, .description = "add author",
     .sql  = "ALTER TABLE notes ADD COLUMN author TEXT NOT NULL DEFAULT ''",
     .down = "ALTER TABLE notes DROP COLUMN author"},
    {.version = 3, .description = "seed welcome note",  // code, not SQL —
     .down = "DELETE FROM notes WHERE author = 'system'",  // down still records
     .fn = [](salt::db& d) -> salt::result<void> {
       return d.exec("INSERT INTO notes (body, author) VALUES (?, ?)",
                     std::string("welcome"), std::string("system"));
     }},
};

int main() {
  salt::db db = need(salt::sqlite::open_memory());

  // First run applies everything; every later run proves history instead.
  auto r1 = need(salt::migrate(db, kSchema));
  std::println("run 1: applied={} validated={}", r1.applied, r1.validated);
  auto r2 = need(salt::migrate(db, kSchema));
  std::println("run 2: applied={} validated={}", r2.applied, r2.validated);

  // validate() is the same proof without applying — for readiness checks.
  auto v = need(salt::validate(db, kSchema));
  std::println("validate: proven={} pending={}", v.validated, v.pending);

  // History is a table like any other, applied_at in sortable UTC.
  for (const auto& row : need(db.query<salt::schema_history>("ORDER BY version")))
    std::println("  v{} '{}' at {} (down: {})", row.version, row.description,
                 row.applied_at, row.down_sql.has_value() ? "yes" : "no");

  // Roll back to version 1 using the down SQL recorded at apply time.
  std::println("rollback to v1 unwound {} migrations",
               need(salt::rollback(db, 1)));
  need(salt::migrate(db, kSchema));  // and forward again

  // A new deployment that dropped v3 from its changelog: refused by
  // default, unwound from history's own down SQL when opted in.
  constexpr salt::migration kOlder[] = {kSchema[0], kSchema[1]};
  if (auto refuse = salt::migrate(db, kOlder); !refuse)
    std::println("v3 unknown to this binary: {}", refuse.error().message);
  auto un = need(salt::migrate(db, kOlder, {.unwind_missing = true}));
  std::println("with unwind_missing: unwound={} validated={}",
               un.unwound, un.validated);
}
