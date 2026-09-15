// PostgreSQL server suite: the conformance contract against a live server,
// plus everything Postgres-specific — RETURNING ids, live $n / ?? adaptation,
// Oid text shaping, transactional DDL in migrations, lock-conflict errors.
//
// Connection from SALT_PG_DSN (default matches tests/containers/
// docker-compose.yml). No reachable server → exit 77 = ctest SKIP.
// Each run works inside its own schema, dropped on exit, so reruns are clean.

#include <saltherring/saltherring.hpp>
#include <saltherring/pg.hpp>

#include "conformance.hpp"
#include "server_fixture.hpp"

#include <format>
#include <string>
#include <unistd.h>

namespace {

std::string g_dsn;
std::string g_schema;

// A fresh connection, scoped into this run's scratch schema.
salt::db fresh() {
  auto d = salt::pg::open(g_dsn.c_str());
  if (!d) servertest::skip(std::format("postgres not reachable: {}",
                                       d.error().message));
  auto sp = d->exec(salt::unchecked_sql{"SET search_path TO " + g_schema});
  if (!sp) servertest::skip("could not set search_path");
  return std::move(*d);
}

struct [[=salt::table("pg_users")]] PgUser {
  [[=salt::auto_pk]] std::int64_t id = 0;
  std::string name;
};

// --- RETURNING: the id comes from the INSERT row, never last_insert_id -----

void test_returning() {
  salt::db db = fresh();
  EXPECT_OK(db.create_table<PgUser>());
  PgUser a{.name = "a"}, b{.name = "b"};
  EXPECT_OK(db.insert(a));
  EXPECT_OK(db.insert(b));
  EXPECT(a.id != 0);
  EXPECT(b.id == a.id + 1);
  EXPECT(!db.raw().last_insert_id());  // unreachable by design on postgres
}

// --- live placeholder adaptation: $n, comments, dollar quotes, ?? -----------

void test_adaptation_live() {
  salt::db db = fresh();
  EXPECT_OK(db.create_table<PgUser>());
  PgUser u{.name = "quoted?name"};
  EXPECT_OK(db.insert(u));

  // ? inside comments and literals reaches the server untouched; the real
  // placeholder becomes $1.
  auto q = db.query<PgUser>("WHERE name = ? -- is it ?\n", std::string("quoted?name"));
  EXPECT_OK(q);
  EXPECT_EQ(q->size(), 1u);

  // Dollar-quoted function body straight through exec.
  EXPECT_OK(db.exec("CREATE FUNCTION sq(x INTEGER) RETURNS INTEGER AS "
                    "$$ SELECT x * x $$ LANGUAGE SQL"));
  EXPECT_EQ(*db.scalar<std::int64_t>("SELECT sq(7)"), 49);

  // ?? escapes to the JSON ? operator — the reason the escape exists.
  auto has_key = db.scalar<bool>("SELECT '{\"k\": 1}'::jsonb ?? 'k'");
  EXPECT_OK(has_key);
  EXPECT(has_key.value_or(false));
  auto no_key = db.scalar<bool>("SELECT '{\"k\": 1}'::jsonb ?? 'missing'");
  EXPECT_OK(no_key);
  EXPECT(!no_key.value_or(true));
}

// --- text-protocol shaping by Oid -------------------------------------------

void test_oid_shaping() {
  salt::db db = fresh();
  EXPECT_EQ(*db.scalar<std::int64_t>("SELECT 42::int2"), 42);
  EXPECT_EQ(*db.scalar<std::int64_t>("SELECT 42::int4"), 42);
  EXPECT_EQ(*db.scalar<std::int64_t>("SELECT -9223372036854775807::int8"),
            -9223372036854775807);
  EXPECT_EQ(*db.scalar<double>("SELECT 1.5::float4"), 1.5);
  EXPECT_EQ(*db.scalar<double>("SELECT 1.5::numeric"), 1.5);  // lossy by policy
  EXPECT_EQ(*db.scalar<bool>("SELECT TRUE"), true);
  EXPECT_EQ(*db.scalar<bool>("SELECT FALSE"), false);
  auto bytes = db.scalar<std::vector<std::uint8_t>>("SELECT '\\xdeadbeef'::bytea");
  EXPECT_OK(bytes);
  EXPECT((*bytes == std::vector<std::uint8_t>{0xde, 0xad, 0xbe, 0xef}));
}

// --- isolation options render and are enforced -------------------------------

void test_isolation() {
  salt::db db = fresh();
  EXPECT_OK(db.create_table<PgUser>());
  auto base = db.count<PgUser>().value_or(-1);  // schema is shared per run

  EXPECT_OK(db.transaction([&]() -> salt::result<void> {
    PgUser u{.name = "serializable"};
    return db.insert(u).transform([](std::int64_t) {});
  }, {.iso = salt::isolation::serializable}));
  EXPECT_EQ(*db.count<PgUser>(), base + 1);

  // A read-only transaction cannot write; the guard rolls back and the
  // connection stays usable.
  auto ro = db.transaction([&]() -> salt::result<void> {
    PgUser u{.name = "nope"};
    return db.insert(u).transform([](std::int64_t) {});
  }, {.iso = salt::isolation::repeatable_read, .read_only = true});
  EXPECT(!ro);
  EXPECT_EQ(*db.count<PgUser>(), base + 1);
}

// --- migrations: transactional DDL means a failed migration leaves nothing --

void test_migrations() {
  salt::db db = fresh();

  const salt::migration good[] = {
      {.version = 1, .description = "create notes",
       .sql = "CREATE TABLE notes (id BIGSERIAL PRIMARY KEY, body TEXT NOT NULL)",
       .down = "DROP TABLE notes"},
      {.version = 2, .description = "add author",
       .sql = "ALTER TABLE notes ADD COLUMN author TEXT NOT NULL DEFAULT ''",
       .down = "ALTER TABLE notes DROP COLUMN author"},
  };
  auto r = salt::migrate(db, good);
  EXPECT_OK(r);
  EXPECT_EQ(r->applied, 2);
  EXPECT_EQ(salt::migrate(db, good)->validated, 2);
  EXPECT_OK(db.exec("INSERT INTO notes (body, author) VALUES ('hi', 'hs')"));

  // Postgres DDL is transactional: the partial work of a failed migration
  // does not exist afterwards.
  const salt::migration partial[] = {
      good[0], good[1],
      {.version = 3, .description = "half done",
       .sql = "CREATE TABLE mtx (x INTEGER); INSERT INTO mtx VALUES (1); NOT SQL"}};
  EXPECT(!salt::migrate(db, partial));
  EXPECT(!db.scalar<std::int64_t>("SELECT COUNT(*) FROM mtx"));  // no such table

  // Tampered history is refused before its down SQL could run.
  EXPECT_OK(db.exec("UPDATE salt_schema_history SET down_sql = 'DROP TABLE notes' "
                    "WHERE version = 2"));
  auto rb = salt::rollback(db, 1);
  EXPECT(!rb);
  EXPECT(rb.error().code == salt::errc::migration_tampered);
  EXPECT_OK(db.exec("SELECT author FROM notes"));  // column still there
}

// --- lock conflicts surface as coded errors, not hangs ----------------------

void test_lock_conflict() {
  salt::db a = fresh();
  salt::db b = fresh();
  EXPECT_OK(a.create_table<PgUser>());
  PgUser u{.name = "contested"};
  EXPECT_OK(a.insert(u));

  EXPECT_OK(b.exec("SET lock_timeout = '500ms'"));
  EXPECT_OK(a.exec("BEGIN"));
  EXPECT_OK(a.execute("UPDATE pg_users SET name = 'a holds it' WHERE id = ?", u.id));
  auto blocked = b.execute("UPDATE pg_users SET name = 'b wants it' WHERE id = ?", u.id);
  EXPECT(!blocked);
  EXPECT(blocked.error().message.contains("lock timeout"));
  EXPECT_OK(a.exec("ROLLBACK"));
  // b's connection is fine afterwards.
  EXPECT_OK(b.execute("UPDATE pg_users SET name = 'b got it' WHERE id = ?", u.id));
}

}  // namespace

int main() {
  g_dsn = servertest::env_or(
      "SALT_PG_DSN",
      "host=127.0.0.1 port=15432 dbname=salt_test user=salt password=salt");
  g_schema = std::format("salt_run_{}", getpid());

  {  // scratch schema for this run; also the reachability probe
    auto d = salt::pg::open(g_dsn.c_str());
    if (!d) servertest::skip(std::format("postgres not reachable: {}",
                                         d.error().message));
    if (auto r = d->exec(salt::unchecked_sql{"CREATE SCHEMA " + g_schema}); !r)
      servertest::skip(std::format("cannot create scratch schema: {}",
                                   r.error().message));
  }

  failures += salt::conformance::run([] { return fresh(); });
  test_returning();
  test_adaptation_live();
  test_oid_shaping();
  test_isolation();
  test_migrations();
  test_lock_conflict();

  {  // best-effort cleanup
    auto d = salt::pg::open(g_dsn.c_str());
    if (d) (void)d->exec(salt::unchecked_sql{"DROP SCHEMA " + g_schema + " CASCADE"});
  }
  return servertest::finish("postgres");
}
