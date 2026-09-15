#include <saltherring/saltherring.hpp>
#include <saltherring/sqlite.hpp>

#include <cstdint>
#include <optional>
#include <print>
#include <string>
#include <vector>

static int failures = 0;

#define EXPECT(cond)                                                     \
  do {                                                                   \
    if (!(cond)) {                                                       \
      ++failures;                                                        \
      std::println("FAIL {}:{}  {}", __FILE__, __LINE__, #cond);         \
    }                                                                    \
  } while (0)

#define EXPECT_EQ(a, b)                                                  \
  do {                                                                   \
    auto va = (a);                                                       \
    auto vb = (b);                                                       \
    if (!(va == vb)) {                                                   \
      ++failures;                                                        \
      std::println("FAIL {}:{}  {} == {}\n  lhs: {}\n  rhs: {}",         \
                   __FILE__, __LINE__, #a, #b, va, vb);                  \
    }                                                                    \
  } while (0)

#define EXPECT_OK(expr)                                                  \
  do {                                                                   \
    auto&& _r = (expr);                                                  \
    if (!_r) {                                                           \
      ++failures;                                                        \
      std::println("FAIL {}:{}  {} errored: {}", __FILE__, __LINE__,     \
                   #expr, _r.error().message);                           \
    }                                                                    \
  } while (0)

// --- fixtures ---------------------------------------------------------------

struct [[=salt::table("users")]] User {
  [[=salt::auto_pk]] std::int64_t id = 0;
  [[=salt::unique{}]] std::string email;
  std::string name;
  [[=salt::check("balance >= 0")]] double balance = 0;
  bool active = false;
  std::optional<std::string> nickname;
  [[=salt::transient{}]] int cache = -1;
};

// sardine annotations govern the wire name of enumerators — the column
// stores exactly what sardine would put in JSON.
enum class [[=sardine::rename_all("SCREAMING_SNAKE_CASE")]] Level {
  debug_info,
  warning,
  fatal_error,
};

struct Address {  // becomes a JSON TEXT column, via sardine
  std::string street;
  std::string city;
};

struct [[=salt::table("events")]] Event {
  [[=salt::auto_pk]] std::int64_t id = 0;
  [[=salt::indexed{}]] Level level = Level::warning;
  Address addr;
  std::vector<int> readings;
  std::vector<std::uint8_t> payload;  // BLOB, sardine's byte-sequence rule
};

struct Setting {  // no salt::table — default name: pascal → snake
  [[=salt::pk{}]] std::string key;
  std::string value;
};

struct [[=salt::table("orders")]] Order {
  [[=salt::auto_pk]] std::int64_t id = 0;
  [[=salt::references("users")]] std::int64_t user_id = 0;
  [[=salt::sql_default("0")]] std::int64_t items = 0;
};

struct AccountEntry {
  [[=salt::pk{}]] std::int64_t id = 0;
  std::int64_t amount = 0;
};

// --- model / DDL ------------------------------------------------------------

static void test_model() {
  static_assert(salt::table_of<User>() == "users");
  static_assert(salt::table_of<Setting>() == "setting");
  static_assert(salt::table_of<AccountEntry>() == "account_entry");

  constexpr auto cols = salt::columns_of<User>();
  static_assert(cols.size() == 6);  // transient cache excluded
  static_assert(cols[0].is_pk && cols[0].auto_inc);
  static_assert(cols[1].is_unique);
  static_assert(cols[5].nullable);

  EXPECT_EQ(salt::create_table_sql<User>(salt::sqlite_dialect),
            "CREATE TABLE IF NOT EXISTS \"users\" ("
            "\"id\" INTEGER PRIMARY KEY AUTOINCREMENT, "
            "\"email\" TEXT NOT NULL UNIQUE, "
            "\"name\" TEXT NOT NULL, "
            "\"balance\" REAL NOT NULL CHECK (balance >= 0), "
            "\"active\" INTEGER NOT NULL, "
            "\"nickname\" TEXT)");

  EXPECT_EQ(salt::create_table_sql<User>(salt::postgres_dialect),
            "CREATE TABLE IF NOT EXISTS \"users\" ("
            "\"id\" BIGSERIAL PRIMARY KEY, "
            "\"email\" TEXT NOT NULL UNIQUE, "
            "\"name\" TEXT NOT NULL, "
            "\"balance\" DOUBLE PRECISION NOT NULL CHECK (balance >= 0), "
            "\"active\" BOOLEAN NOT NULL, "
            "\"nickname\" TEXT)");

  EXPECT_EQ(salt::create_table_sql<Setting>(salt::mariadb_dialect),
            "CREATE TABLE IF NOT EXISTS `setting` ("
            "`key` VARCHAR(255) PRIMARY KEY, "
            "`value` TEXT NOT NULL)");

  // Foreign key + DEFAULT.
  EXPECT_EQ(salt::create_table_sql<Order>(salt::sqlite_dialect),
            "CREATE TABLE IF NOT EXISTS \"orders\" ("
            "\"id\" INTEGER PRIMARY KEY AUTOINCREMENT, "
            "\"user_id\" INTEGER NOT NULL REFERENCES \"users\"(\"id\"), "
            "\"items\" INTEGER NOT NULL DEFAULT 0)");

  // Indexed column rides along as a second statement.
  EXPECT(salt::create_table_sql<Event>(salt::sqlite_dialect).contains(
      "CREATE INDEX IF NOT EXISTS \"idx_events_level\" ON \"events\" (\"level\")"));

  // Postgres numbers its placeholders and asks for the id back.
  EXPECT_EQ(salt::detail::insert_sql<User>(salt::postgres_dialect),
            "INSERT INTO \"users\" (\"email\", \"name\", \"balance\", "
            "\"active\", \"nickname\") VALUES ($1, $2, $3, $4, $5) "
            "RETURNING \"id\"");
  EXPECT_EQ(salt::detail::update_sql<Setting>(salt::postgres_dialect),
            "UPDATE \"setting\" SET \"value\" = $1 WHERE \"key\" = $2");
  EXPECT_EQ(salt::detail::adapt_placeholders(
                "WHERE a = ? AND b = 'lit?eral' AND c = ?", salt::postgres_dialect),
            "WHERE a = $1 AND b = 'lit?eral' AND c = $2");
}

// --- CRUD against SQLite ----------------------------------------------------

static void test_crud() {
  auto dbr = salt::sqlite::open_memory();
  EXPECT_OK(dbr);
  salt::db db = std::move(*dbr);

  EXPECT_OK((db.create_tables<User, Event, Setting, Order>()));

  User u{.email = "henri@example.com", .name = "Henri", .balance = 12.5,
         .active = true};
  auto id = db.insert(u);
  EXPECT_OK(id);
  EXPECT_EQ(u.id, *id);  // auto pk written back
  EXPECT(u.id != 0);

  User v{.email = "someone@example.com", .name = "Someone", .balance = 1,
         .nickname = "so"};
  EXPECT_OK(db.insert(v));

  auto found = db.find<User>(u.id);
  EXPECT_OK(found);
  EXPECT(found->has_value());
  EXPECT_EQ((*found)->email, "henri@example.com");
  EXPECT_EQ((*found)->balance, 12.5);
  EXPECT_EQ((*found)->active, true);
  EXPECT(!(*found)->nickname.has_value());
  EXPECT_EQ((*found)->cache, -1);  // transient untouched

  auto missing = db.find<User>(9999);
  EXPECT_OK(missing);
  EXPECT(!missing->has_value());

  (*found)->balance = 99.0;
  (*found)->nickname = "hs";
  EXPECT_OK(db.update(**found));
  auto again = db.find<User>(u.id);
  EXPECT_EQ((*again)->balance, 99.0);
  EXPECT_EQ((*again)->nickname.value_or(""), "hs");

  auto rich = db.query<User>("WHERE balance > ? ORDER BY balance DESC", 0.5);
  EXPECT_OK(rich);
  EXPECT_EQ(rich->size(), 2u);
  EXPECT_EQ(rich->front().balance, 99.0);

  auto n = db.count<User>();
  EXPECT_OK(n);
  EXPECT_EQ(*n, 2);

  auto one = db.query_one<User>("WHERE email = ?", std::string("someone@example.com"));
  EXPECT_OK(one);
  EXPECT(one->has_value());

  EXPECT_OK(db.erase<User>(v.id));
  EXPECT_EQ(*db.count<User>(), 1);

  // UNIQUE violation surfaces as errc::exec, with a message.
  User dup{.email = "henri@example.com", .name = "Dup"};
  auto bad = db.insert(dup);
  EXPECT(!bad);
  EXPECT(bad.error().code == salt::errc::exec);

  // CHECK violation.
  User neg{.email = "neg@example.com", .name = "Neg", .balance = -1};
  EXPECT(!db.insert(neg));

  // Foreign key enforcement (PRAGMA foreign_keys is on).
  Order o{.user_id = 424242};
  EXPECT(!db.insert(o));
  o.user_id = u.id;
  EXPECT_OK(db.insert(o));

  // Text primary key.
  Setting s{.key = "theme", .value = "dark"};
  EXPECT_OK(db.insert(s));
  auto theme = db.find<Setting>(std::string("theme"));
  EXPECT_OK(theme);
  EXPECT_EQ((*theme)->value, "dark");
}

// --- sardine-compatible columns ---------------------------------------------

static void test_sardine_columns() {
  salt::db db = std::move(*salt::sqlite::open_memory());
  EXPECT_OK(db.create_table<Event>());

  Event e{.level = Level::fatal_error,
          .addr = {.street = "Mannerheimintie 1", .city = "Helsinki"},
          .readings = {1, 2, 3},
          .payload = {0xde, 0xad, 0xbe, 0xef}};
  EXPECT_OK(db.insert(e));

  // The enum column holds the sardine wire name, not a number.
  auto raw = db.scalar<std::string>("SELECT level FROM events WHERE id = ?", e.id);
  EXPECT_OK(raw);
  EXPECT_EQ(*raw, "FATAL_ERROR");

  // The struct column holds sardine JSON.
  auto j = db.scalar<std::string>("SELECT addr FROM events WHERE id = ?", e.id);
  EXPECT_OK(j);
  EXPECT_EQ(*j, sardine::to_json(e.addr));
  auto parsed = sardine::from_json<Address>(*j);
  EXPECT_OK(parsed);
  EXPECT_EQ(parsed->city, "Helsinki");

  auto back = db.find<Event>(e.id);
  EXPECT_OK(back);
  EXPECT((*back)->level == Level::fatal_error);
  EXPECT_EQ((*back)->addr.street, "Mannerheimintie 1");
  EXPECT(((*back)->readings == std::vector<int>{1, 2, 3}));
  EXPECT(((*back)->payload == std::vector<std::uint8_t>{0xde, 0xad, 0xbe, 0xef}));

  // Enum text that names no enumerator is an unknown_enum error on read.
  EXPECT_OK(db.exec("UPDATE events SET level = 'BOGUS'"));
  auto broken = db.find<Event>(e.id);
  EXPECT(!broken);
  EXPECT(broken.error().code == salt::errc::unknown_enum);
}

// --- transactions -----------------------------------------------------------

static void test_transactions() {
  salt::db db = std::move(*salt::sqlite::open_memory());
  EXPECT_OK(db.create_table<User>());

  auto r = db.transaction([&]() -> salt::result<void> {
    User a{.email = "a@x", .name = "A"};
    if (auto i = db.insert(a); !i) return std::unexpected(i.error());
    return salt::fail(salt::errc::exec, "abort on purpose");
  });
  EXPECT(!r);
  EXPECT_EQ(*db.count<User>(), 0);  // rolled back

  EXPECT_OK(db.transaction([&]() -> salt::result<void> {
    User a{.email = "a@x", .name = "A"};
    return db.insert(a).transform([](std::int64_t) {});
  }));
  EXPECT_EQ(*db.count<User>(), 1);
}

// --- migrations -------------------------------------------------------------

static constexpr salt::migration kV1[] = {
    {.version = 1, .description = "create notes",
     .sql = "CREATE TABLE notes (id INTEGER PRIMARY KEY, body TEXT NOT NULL)"},
};

static constexpr salt::migration kV2[] = {
    kV1[0],
    {.version = 2, .description = "add author",
     .sql = "ALTER TABLE notes ADD COLUMN author TEXT NOT NULL DEFAULT ''"},
};

static void test_migrations() {
  salt::db db = std::move(*salt::sqlite::open_memory());

  auto r1 = salt::migrate(db, kV1);
  EXPECT_OK(r1);
  EXPECT_EQ(r1->applied, 1);
  EXPECT_EQ(r1->validated, 0);

  // Re-running is a no-op that validates history.
  auto r1b = salt::migrate(db, kV1);
  EXPECT_OK(r1b);
  EXPECT_EQ(r1b->applied, 0);
  EXPECT_EQ(r1b->validated, 1);

  auto r2 = salt::migrate(db, kV2);
  EXPECT_OK(r2);
  EXPECT_EQ(r2->applied, 1);
  EXPECT_EQ(r2->validated, 1);
  EXPECT_OK(db.exec("INSERT INTO notes (body, author) VALUES ('hi', 'hs')"));

  // History is queryable like any other table.
  auto hist = db.query<salt::schema_history>("ORDER BY version");
  EXPECT_OK(hist);
  EXPECT_EQ(hist->size(), 2u);
  EXPECT_EQ(hist->at(1).description, "add author");
  EXPECT(!hist->at(0).applied_at.empty());

  // Tampering with an applied migration's SQL is caught by the checksum.
  salt::migration tampered[] = {kV2[0], kV2[1]};
  tampered[0].sql = "CREATE TABLE notes (id INTEGER PRIMARY KEY)";
  auto bad = salt::migrate(db, tampered);
  EXPECT(!bad);
  EXPECT(bad.error().code == salt::errc::migration_checksum);

  // A migration list that lost an applied version is an error.
  const salt::migration onlyV2[] = {kV2[1]};
  auto missing = salt::migrate(db, onlyV2);
  EXPECT(!missing);
  EXPECT(missing.error().code == salt::errc::migration_missing);

  // A pending version older than an applied one is an error.
  salt::migration outOfOrder[] = {
      kV2[0], kV2[1],
      {.version = 0, .description = "too late", .sql = "SELECT 1"}};
  auto ooo = salt::migrate(db, outOfOrder);
  EXPECT(!ooo);
  EXPECT(ooo.error().code == salt::errc::migration_order);

  // Duplicate versions are rejected before anything runs.
  salt::migration dup[] = {kV1[0], kV1[0]};
  auto d = salt::migrate(db, dup);
  EXPECT(!d);
  EXPECT(d.error().code == salt::errc::migration_duplicate);

  // A failing migration rolls back and reports its version.
  salt::migration failing[] = {
      kV2[0], kV2[1],
      {.version = 3, .description = "broken", .sql = "NOT VALID SQL"}};
  auto f = salt::migrate(db, failing);
  EXPECT(!f);
  EXPECT(f.error().message.contains("migration 3"));
  EXPECT_EQ(salt::migrate(db, kV2)->validated, 2);  // history unharmed

  // Code migrations: a function instead of SQL.
  salt::migration withCode[] = {
      kV2[0], kV2[1],
      {.version = 3, .description = "seed", .fn = [](salt::db& d) -> salt::result<void> {
         return d.exec("INSERT INTO notes (body) VALUES ('seeded')");
       }}};
  auto c = salt::migrate(db, withCode);
  EXPECT_OK(c);
  EXPECT_EQ(c->applied, 1);
  EXPECT_EQ(*db.scalar<std::int64_t>("SELECT COUNT(*) FROM notes"), 2);
}

// --- changelog control: validate / rollback / unwind ------------------------

static void test_changelog_control() {
  salt::db db = std::move(*salt::sqlite::open_memory());
  const salt::migration v3[] = {
      {.version = 1, .description = "create notes",
       .sql = "CREATE TABLE notes (id INTEGER PRIMARY KEY, body TEXT NOT NULL)",
       .down = "DROP TABLE notes"},
      {.version = 2, .description = "add author",
       .sql = "ALTER TABLE notes ADD COLUMN author TEXT NOT NULL DEFAULT ''",
       .down = "ALTER TABLE notes DROP COLUMN author"},
      {.version = 3, .description = "author index",
       .sql = "CREATE INDEX idx_notes_author ON notes (author)",
       .down = "DROP INDEX idx_notes_author"},
  };

  // validate() proves without applying.
  auto v0 = salt::validate(db, v3);
  EXPECT_OK(v0);
  EXPECT_EQ(v0->validated, 0);
  EXPECT_EQ(v0->pending, 3);
  EXPECT_EQ(*db.count<salt::schema_history>(), 0);  // nothing was applied

  EXPECT_OK(salt::migrate(db, v3));
  auto v1 = salt::validate(db, v3);
  EXPECT_OK(v1);
  EXPECT_EQ(v1->validated, 3);
  EXPECT_EQ(v1->pending, 0);

  // The down SQL is recorded in history, verbatim.
  auto hist = db.query<salt::schema_history>("ORDER BY version");
  EXPECT_OK(hist);
  EXPECT_EQ(hist->at(0).down_sql.value_or(""), "DROP TABLE notes");

  // validate catches tampering without touching the database.
  salt::migration tampered[] = {v3[0], v3[1], v3[2]};
  tampered[1].sql = "ALTER TABLE notes ADD COLUMN writer TEXT";
  auto vt = salt::validate(db, tampered);
  EXPECT(!vt);
  EXPECT(vt.error().code == salt::errc::migration_checksum);

  // rollback(db, 1) unwinds 3 then 2 from the recorded down SQL.
  auto rb = salt::rollback(db, 1);
  EXPECT_OK(rb);
  EXPECT_EQ(*rb, 2);
  EXPECT_EQ(*db.count<salt::schema_history>(), 1);
  EXPECT_OK(db.exec("INSERT INTO notes (body) VALUES ('x')"));
  EXPECT(!db.exec("INSERT INTO notes (body, author) VALUES ('x', 'y')"));

  auto again = salt::migrate(db, v3);
  EXPECT_OK(again);
  EXPECT_EQ(again->applied, 2);

  // A new deployment that dropped v3 from its list: an error by default,
  // an unwind (using history's own down SQL) when opted in.
  const salt::migration v2only[] = {v3[0], v3[1]};
  auto refuse = salt::migrate(db, v2only);
  EXPECT(!refuse);
  EXPECT(refuse.error().code == salt::errc::migration_missing);
  auto un = salt::migrate(db, v2only, {.unwind_missing = true});
  EXPECT_OK(un);
  EXPECT_EQ(un->unwound, 1);
  EXPECT_EQ(un->validated, 2);
  EXPECT_EQ(un->applied, 0);
  EXPECT_EQ(*db.count<salt::schema_history>(), 2);
  EXPECT(!db.exec("DROP INDEX idx_notes_author"));  // the index really is gone

  // A migration that recorded no down SQL cannot be unwound.
  const salt::migration nodown[] = {
      v3[0], v3[1],
      {.version = 4, .description = "no down",
       .sql = "CREATE TABLE t4 (x INTEGER)"}};
  EXPECT_OK(salt::migrate(db, nodown));
  auto bad = salt::rollback(db, 2);
  EXPECT(!bad);
  EXPECT(bad.error().code == salt::errc::migration_no_down);
}

int main() {
  test_model();
  test_crud();
  test_sardine_columns();
  test_transactions();
  test_migrations();
  test_changelog_control();
  if (failures == 0) std::println("all tests passed");
  else std::println("{} FAILURES", failures);
  return failures != 0;
}
