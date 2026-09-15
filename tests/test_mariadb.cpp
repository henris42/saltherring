// MariaDB server suite: the conformance contract against a live server, plus
// everything MariaDB-specific — AUTO_INCREMENT/last_insert_id, matched-rows
// semantics (CLIENT_FOUND_ROWS), utf8mb4, multi-statement draining, and the
// documented DDL-self-commit migration caveat.
//
// Connection from SALT_MARIADB_HOST/PORT/USER/PASSWORD (defaults match
// tests/containers/docker-compose.yml; the user must be able to CREATE
// DATABASE — the compose root user is). No reachable server → exit 77 =
// ctest SKIP. Each run works inside its own database, dropped on exit.

#include <saltherring/saltherring.hpp>
#include <saltherring/mariadb.hpp>

#include "conformance.hpp"
#include "server_fixture.hpp"

#include <format>
#include <string>
#include <unistd.h>

namespace {

std::string g_host, g_user, g_password, g_dbname;
unsigned g_port = 0;

salt::db fresh() {
  auto d = salt::mariadb::open(g_host.c_str(), g_user.c_str(),
                               g_password.c_str(), g_dbname.c_str(), g_port);
  if (!d) servertest::skip(std::format("mariadb not reachable: {}",
                                       d.error().message));
  return std::move(*d);
}

struct [[=salt::table("mdb_users")]] MdbUser {
  [[=salt::auto_pk]] std::int64_t id = 0;
  [[=salt::unique{}]] std::string name;
};

// --- AUTO_INCREMENT ids arrive via last_insert_id ---------------------------

void test_auto_increment() {
  salt::db db = fresh();
  EXPECT_OK(db.create_table<MdbUser>());
  MdbUser a{.name = "a"}, b{.name = "b"};
  EXPECT_OK(db.insert(a));
  EXPECT_OK(db.insert(b));
  EXPECT(a.id != 0);
  EXPECT(b.id == a.id + 1);
}

// --- matched rows, not changed rows (CLIENT_FOUND_ROWS) ----------------------

void test_matched_rows() {
  salt::db db = fresh();
  EXPECT_OK(db.create_table<MdbUser>());
  MdbUser u{.name = "same"};
  EXPECT_OK(db.insert(u));
  // Setting a value that is already there must still count the matched row —
  // MySQL's default would report 0 changed here.
  EXPECT_EQ(db.execute("UPDATE mdb_users SET name = ? WHERE id = ?",
                       std::string("same"), u.id).value_or(-1), 1);
}

// --- multi-statement exec drains every result set ----------------------------

void test_multi_statement() {
  salt::db db = fresh();
  // Statements with result sets in the middle must not wedge the connection.
  EXPECT_OK(db.exec("CREATE TABLE ms (x INTEGER); "
                    "INSERT INTO ms VALUES (1); "
                    "SELECT * FROM ms; "
                    "INSERT INTO ms VALUES (2)"));
  EXPECT_EQ(*db.scalar<std::int64_t>("SELECT COUNT(*) FROM ms"), 2);
}

// --- isolation rendering (SET TRANSACTION ...; START TRANSACTION) -----------

void test_isolation() {
  salt::db db = fresh();
  EXPECT_OK(db.create_table<MdbUser>());
  auto base = db.count<MdbUser>().value_or(-1);  // database is shared per run

  EXPECT_OK(db.transaction([&]() -> salt::result<void> {
    MdbUser u{.name = "serializable"};
    return db.insert(u).transform([](std::int64_t) {});
  }, {.iso = salt::isolation::serializable}));
  EXPECT_EQ(*db.count<MdbUser>(), base + 1);

  auto ro = db.transaction([&]() -> salt::result<void> {
    MdbUser u{.name = "nope"};
    return db.insert(u).transform([](std::int64_t) {});
  }, {.read_only = true});
  EXPECT(!ro);
  EXPECT_EQ(*db.count<MdbUser>(), base + 1);
}

// --- migrations: DDL self-commits — the Flyway caveat, made executable ------

void test_migrations() {
  salt::db db = fresh();

  const salt::migration good[] = {
      {.version = 1, .description = "create notes",
       .sql = "CREATE TABLE notes (id BIGINT PRIMARY KEY AUTO_INCREMENT, "
              "body TEXT NOT NULL)",
       .down = "DROP TABLE notes"},
  };
  auto r = salt::migrate(db, good);
  EXPECT_OK(r);
  EXPECT_EQ(r->applied, 1);
  EXPECT_EQ(salt::migrate(db, good)->validated, 1);

  // Unlike Postgres, a failed migration's DDL stays: mtx exists even though
  // the migration errored and history records nothing. This is the
  // documented MariaDB caveat — asserted here so a behavior change is news.
  const salt::migration partial[] = {
      good[0],
      {.version = 2, .description = "half done",
       .sql = "CREATE TABLE mtx (x INTEGER); NOT SQL"}};
  EXPECT(!salt::migrate(db, partial));
  EXPECT_OK(db.exec("SELECT * FROM mtx"));                       // DDL committed
  EXPECT_EQ(*db.count<salt::schema_history>(), 1);               // v2 not recorded

  // Tampered history refused before its down SQL runs.
  EXPECT_OK(db.exec("UPDATE salt_schema_history "
                    "SET down_sql = 'DROP TABLE notes; DROP TABLE mtx' "
                    "WHERE version = 1"));
  auto rb = salt::rollback(db, 0);
  EXPECT(!rb);
  if (!rb) EXPECT(rb.error().code == salt::errc::migration_tampered);
  EXPECT_OK(db.exec("SELECT * FROM notes"));  // still there
}

// --- lock conflicts surface as coded errors, not hangs ----------------------

void test_lock_conflict() {
  salt::db a = fresh();
  salt::db b = fresh();
  EXPECT_OK(a.create_table<MdbUser>());
  MdbUser u{.name = "contested"};
  EXPECT_OK(a.insert(u));

  EXPECT_OK(b.exec("SET innodb_lock_wait_timeout = 1"));
  EXPECT_OK(a.exec("BEGIN"));
  EXPECT_OK(a.execute("UPDATE mdb_users SET name = 'a holds it' WHERE id = ?", u.id));
  auto blocked = b.execute("UPDATE mdb_users SET name = 'b wants it' WHERE id = ?", u.id);
  EXPECT(!blocked);
  EXPECT_OK(a.exec("ROLLBACK"));
  EXPECT_OK(b.execute("UPDATE mdb_users SET name = 'b got it' WHERE id = ?", u.id));
}

}  // namespace

int main() {
  g_host = servertest::env_or("SALT_MARIADB_HOST", "127.0.0.1");
  g_port = unsigned(std::stoul(servertest::env_or("SALT_MARIADB_PORT", "33069")));
  g_user = servertest::env_or("SALT_MARIADB_USER", "root");
  g_password = servertest::env_or("SALT_MARIADB_PASSWORD", "root");
  g_dbname = std::format("salt_run_{}", getpid());

  {  // scratch database for this run; also the reachability probe
    auto d = salt::mariadb::open(g_host.c_str(), g_user.c_str(),
                                 g_password.c_str(), "mysql", g_port);
    if (!d) servertest::skip(std::format("mariadb not reachable: {}",
                                         d.error().message));
    if (auto r = d->exec(salt::unchecked_sql{"CREATE DATABASE " + g_dbname}); !r)
      servertest::skip(std::format("cannot create scratch database: {}",
                                   r.error().message));
  }

  failures += salt::conformance::run([] { return fresh(); });
  test_auto_increment();
  test_matched_rows();
  test_multi_statement();
  test_isolation();
  test_migrations();
  test_lock_conflict();

  {  // best-effort cleanup
    auto d = salt::mariadb::open(g_host.c_str(), g_user.c_str(),
                                 g_password.c_str(), "mysql", g_port);
    if (d) (void)d->exec(salt::unchecked_sql{"DROP DATABASE " + g_dbname});
  }
  return servertest::finish("mariadb");
}
