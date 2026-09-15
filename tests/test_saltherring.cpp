#include <saltherring/saltherring.hpp>
#include <saltherring/sqlite.hpp>

#include "conformance.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <print>
#include <stdexcept>
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

struct [[=salt::table("gauges")]] Gauge {
  [[=salt::auto_pk]] std::int64_t id = 0;
  std::int8_t small = 0;
  std::uint32_t wide = 0;
  std::chrono::sys_seconds at{};  // INTEGER column, epoch seconds
};

struct [[=salt::table("secrets")]] Secret {
  [[=salt::auto_pk]] std::int64_t id = 0;
  [[=salt::sensitive{}]] Level level = Level::warning;
  Level plain = Level::warning;  // contrast: its bad values may ride in detail
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
            "`value` LONGTEXT NOT NULL)");

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

  // Oracle: :n placeholders, identity pk, NUMBER/BINARY_DOUBLE/VARCHAR2.
  // Identifiers are quoted UPPERCASE so unquoted tails (which Oracle folds
  // to upper) keep matching.
  EXPECT_EQ(salt::create_table_sql<User>(salt::oracle_dialect),
            "CREATE TABLE IF NOT EXISTS \"USERS\" ("
            "\"ID\" NUMBER(19) GENERATED BY DEFAULT ON NULL AS IDENTITY "
            "PRIMARY KEY, "
            "\"EMAIL\" VARCHAR2(4000) NOT NULL UNIQUE, "
            "\"NAME\" VARCHAR2(4000) NOT NULL, "
            "\"BALANCE\" BINARY_DOUBLE NOT NULL CHECK (balance >= 0), "
            "\"ACTIVE\" NUMBER(1) NOT NULL, "
            "\"NICKNAME\" VARCHAR2(4000))");
  EXPECT_EQ(salt::detail::insert_sql<User>(salt::oracle_dialect),
            "INSERT INTO \"USERS\" (\"EMAIL\", \"NAME\", \"BALANCE\", "
            "\"ACTIVE\", \"NICKNAME\") VALUES (:1, :2, :3, :4, :5) "
            "RETURNING \"ID\"");
  EXPECT_EQ(salt::detail::update_sql<Setting>(salt::postgres_dialect),
            "UPDATE \"setting\" SET \"value\" = $1 WHERE \"key\" = $2");
}

// --- placeholder rewriting is a tokenizer, not a quote toggle ---------------

static void test_tokenizer() {
  using salt::detail::adapt_placeholders;
  const salt::dialect& pg = salt::postgres_dialect;

  auto adapt = [&](std::string_view in) -> std::string {
    auto r = adapt_placeholders(in, pg);
    return r ? *r : "<error: " + r.error().message + ">";
  };

  EXPECT_EQ(adapt("WHERE a = ? AND b = 'lit?eral' AND c = ?"),
            "WHERE a = $1 AND b = 'lit?eral' AND c = $2");
  EXPECT_EQ(adapt("'it''s ?' ?"), "'it''s ?' $1");        // '' doubling
  EXPECT_EQ(adapt("\"col?\" = ?"), "\"col?\" = $1");      // quoted identifier
  EXPECT_EQ(adapt("`col?` = ?"), "`col?` = $1");          // backtick identifier
  EXPECT_EQ(adapt("-- ?\nWHERE x = ?"), "-- ?\nWHERE x = $1");
  EXPECT_EQ(adapt("SELECT 1 -- trailing ?"), "SELECT 1 -- trailing ?");
  EXPECT_EQ(adapt("/* ? */ ?"), "/* ? */ $1");
  EXPECT_EQ(adapt("/* a /* nested ? */ b */ ?"), "/* a /* nested ? */ b */ $1");
  EXPECT_EQ(adapt("$$ ? $$ ?"), "$$ ? $$ $1");            // dollar-quoted string
  EXPECT_EQ(adapt("$fn$ body ? $fn$ ?"), "$fn$ body ? $fn$ $1");
  // ?? escapes a literal ? in code position (Postgres JSON operators).
  EXPECT_EQ(adapt("data ?? 'k' AND x = ?"), "data ? 'k' AND x = $1");

  // Unbalanced constructs are an error, not a guess.
  auto uq = adapt_placeholders("'abc", pg);
  EXPECT(!uq && uq.error().code == salt::errc::prepare);
  auto uc = adapt_placeholders("/* abc", pg);
  EXPECT(!uc && uc.error().code == salt::errc::prepare);
  auto ud = adapt_placeholders("$tag$ never closed", pg);
  EXPECT(!ud && ud.error().code == salt::errc::prepare);

  // question-style dialects keep ? but still honor the ?? escape and states.
  auto lite = adapt_placeholders("a ?? b = ?", salt::sqlite_dialect);
  EXPECT_OK(lite);
  if (lite) EXPECT_EQ(*lite, "a ? b = ?");

  // Oracle numbers with colons.
  auto ora = adapt_placeholders("WHERE a = ? AND b = 'lit?eral' AND c = ?",
                                salt::oracle_dialect);
  EXPECT_OK(ora);
  if (ora) EXPECT_EQ(*ora, "WHERE a = :1 AND b = 'lit?eral' AND c = :2");
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

  // UNIQUE violation surfaces as errc::constraint — typed, because callers
  // build idioms on the distinction (insert-or-conflict, allocate-retry).
  User dup{.email = "henri@example.com", .name = "Dup"};
  auto bad = db.insert(dup);
  EXPECT(!bad);
  EXPECT(bad.error().code == salt::errc::constraint);

  // CHECK violation is a constraint too.
  User neg{.email = "neg@example.com", .name = "Neg", .balance = -1};
  auto negr = db.insert(neg);
  EXPECT(!negr && negr.error().code == salt::errc::constraint);

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

// --- SQL text hygiene: literal-only tails, single-statement prepare ---------

static void test_sql_hygiene() {
  salt::db db = std::move(*salt::sqlite::open_memory());
  EXPECT_OK(db.create_table<User>());
  User u{.email = "a@x", .name = "A"};
  EXPECT_OK(db.insert(u));

  // Runtime-assembled SQL must announce itself. (tests/nc/ proves the
  // literal-only path cannot take runtime text at all.)
  std::string tail = "WHERE email = ?";
  auto q = db.query<User>(salt::unchecked_sql{tail}, std::string("a@x"));
  EXPECT_OK(q);
  EXPECT_EQ(q->size(), 1u);

  // prepare() takes exactly one statement: a smuggled second one is refused
  // before anything runs.
  auto smuggled = db.exec("UPDATE users SET name = ? ; DELETE FROM users",
                          std::string("x"));
  EXPECT(!smuggled);
  EXPECT(smuggled.error().code == salt::errc::prepare);
  EXPECT_EQ(*db.count<User>(), 1);
  EXPECT_EQ((*db.find<User>(u.id))->name, "A");

  // Multi-statement text is exec()'s job — allowed when nothing is bound.
  EXPECT_OK(db.exec("UPDATE users SET name = 'B'; UPDATE users SET name = 'C'"));
  EXPECT_EQ((*db.find<User>(u.id))->name, "C");

  // Statement text in errors never contains bound values.
  auto bad = db.query<User>("WHERE nocolumn = ?", std::string("secret-value"));
  EXPECT(!bad);
  EXPECT(!bad.error().sql.contains("secret-value"));
  EXPECT(!bad.error().message.contains("secret-value"));
}

// --- integer edges and sys_time columns -------------------------------------

static void test_integer_and_time_columns() {
  salt::db db = std::move(*salt::sqlite::open_memory());
  EXPECT_OK(db.create_table<Gauge>());

  Gauge g{.small = -128, .wide = 4294967295u,
          .at = std::chrono::sys_seconds(std::chrono::seconds(1757900000))};
  EXPECT_OK(db.insert(g));
  auto back = db.find<Gauge>(g.id);
  EXPECT_OK(back);
  EXPECT_EQ(int((*back)->small), -128);
  EXPECT_EQ((*back)->wide, 4294967295u);
  EXPECT((*back)->at == g.at);

  // The time column really is epoch seconds in an INTEGER — sortable and
  // dialect-independent.
  auto epoch = db.scalar<std::int64_t>("SELECT at FROM gauges WHERE id = ?", g.id);
  EXPECT_OK(epoch);
  EXPECT_EQ(*epoch, 1757900000);

  // A stored value the member cannot hold: out_of_range; the value rides in
  // detail, never in the message.
  EXPECT_OK(db.exec("UPDATE gauges SET small = 300"));
  auto broken = db.find<Gauge>(g.id);
  EXPECT(!broken);
  EXPECT(broken.error().code == salt::errc::out_of_range);
  EXPECT_EQ(broken.error().detail, "300");
  EXPECT(!broken.error().message.contains("300"));
}

// --- sensitive columns redact their values from errors ----------------------

static void test_sensitive_redaction() {
  salt::db db = std::move(*salt::sqlite::open_memory());
  EXPECT_OK(db.create_table<Secret>());
  Secret s{.level = Level::fatal_error, .plain = Level::debug_info};
  EXPECT_OK(db.insert(s));

  // A sensitive column's bad value reaches neither message, detail nor sql.
  EXPECT_OK(db.exec("UPDATE secrets SET level = 'SSN-12345'"));
  auto r1 = db.find<Secret>(s.id);
  EXPECT(!r1);
  EXPECT(r1.error().code == salt::errc::unknown_enum);
  EXPECT(r1.error().detail.empty());
  EXPECT(r1.error().message.contains("redacted"));
  EXPECT(!r1.error().message.contains("SSN-12345"));
  EXPECT(!r1.error().sql.contains("SSN-12345"));

  // A plain column keeps the value out of message but offers it in detail.
  EXPECT_OK(db.exec("UPDATE secrets SET level = 'WARNING', plain = 'LEAKY'"));
  auto r2 = db.find<Secret>(s.id);
  EXPECT(!r2);
  EXPECT_EQ(r2.error().detail, "LEAKY");
  EXPECT(!r2.error().message.contains("LEAKY"));
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

  // An exception out of f: the guard rolls back, the exception continues,
  // and the connection is left outside any transaction — fully usable.
  bool threw = false;
  try {
    (void)db.transaction([&]() -> salt::result<void> {
      User x{.email = "x@x", .name = "X"};
      if (auto i = db.insert(x); !i) return std::unexpected(i.error());
      throw std::runtime_error("boom");
    });
  } catch (const std::runtime_error&) {
    threw = true;
  }
  EXPECT(threw);
  EXPECT_EQ(*db.count<User>(), 1);  // X rolled back

  // Next transaction begins cleanly; SQLite write intent declared up front.
  EXPECT_OK(db.transaction([&]() -> salt::result<void> {
    User y{.email = "y@x", .name = "Y"};
    return db.insert(y).transform([](std::int64_t) {});
  }, {.immediate = true}));
  EXPECT_EQ(*db.count<User>(), 2);

  // Nested transactions are savepoints: an inner failure rolls back only
  // the inner work, the outer commit survives.
  EXPECT_OK(db.transaction([&]() -> salt::result<void> {
    User o{.email = "outer@x", .name = "O"};
    if (auto i = db.insert(o); !i) return std::unexpected(i.error());
    auto inner = db.transaction([&]() -> salt::result<void> {
      User n{.email = "inner@x", .name = "N"};
      if (auto i = db.insert(n); !i) return std::unexpected(i.error());
      return salt::fail(salt::errc::exec, "inner abort");
    });
    EXPECT(!inner);
    return {};
  }));
  EXPECT_EQ(*db.count<User>(), 3);
  auto gone = db.query_one<User>("WHERE email = ?", std::string("inner@x"));
  EXPECT_OK(gone);
  EXPECT(!gone->has_value());
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
  EXPECT_EQ(hist->at(0).checksum_rule, 2);
  // applied_at is ISO 8601 UTC — 2026-09-15T12:00:00Z — sortable across hosts.
  const std::string& at0 = hist->at(0).applied_at;
  EXPECT_EQ(at0.size(), 20u);
  EXPECT(at0.size() == 20 && at0[10] == 'T' && at0.back() == 'Z');

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

  // A history row edited in the database fails its own checksum: its stored
  // down SQL is never executed — by rollback() or by unwind_missing.
  EXPECT_OK(db.exec("UPDATE salt_schema_history SET down_sql = 'DROP TABLE t4' "
                    "WHERE version = 4"));
  auto forged = salt::rollback(db, 2);
  EXPECT(!forged);
  EXPECT(forged.error().code == salt::errc::migration_tampered);
  EXPECT_OK(db.exec("INSERT INTO t4 (x) VALUES (1)"));  // t4 was not dropped

  auto forged2 = salt::migrate(db, v2only, {.unwind_missing = true});
  EXPECT(!forged2);
  EXPECT(forged2.error().code == salt::errc::migration_tampered);

  // An unknown checksum rule is refused the same way, even by validate().
  EXPECT_OK(db.exec("UPDATE salt_schema_history SET checksum_rule = 1 "
                    "WHERE version = 2"));
  auto vr = salt::validate(db, nodown);
  EXPECT(!vr);
  EXPECT(vr.error().code == salt::errc::migration_tampered);
}

// --- hardened SQLite open ---------------------------------------------------

static void test_sqlite_hardening() {
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::path dir = fs::temp_directory_path() / "saltherring-hardening-test";
  fs::remove_all(dir, ec);
  fs::create_directories(dir);
  fs::path real = dir / "real.db";
  {
    auto d = salt::sqlite::open(real.c_str());
    EXPECT_OK(d);
    EXPECT_OK(d->exec("CREATE TABLE t (x INTEGER)"));
  }

  // NOFOLLOW by default: a symlinked database path is refused.
  fs::path link = dir / "link.db";
  fs::create_symlink(real, link, ec);
  if (!ec) {
    auto via_link = salt::sqlite::open(link.c_str());
    EXPECT(!via_link);
    EXPECT(via_link.error().code == salt::errc::connect);
    auto followed = salt::sqlite::open(link.c_str(), {.follow_symlinks = true});
    EXPECT_OK(followed);
  }

  // create = false refuses to conjure a missing file.
  auto missing = salt::sqlite::open((dir / "absent.db").c_str(), {.create = false});
  EXPECT(!missing);
  EXPECT(missing.error().code == salt::errc::connect);

  // read_only really is.
  auto ro = salt::sqlite::open(real.c_str(), {.read_only = true});
  EXPECT_OK(ro);
  EXPECT(!ro->exec("INSERT INTO t VALUES (1)"));

  // The hardening pragmas took hold.
  auto d2r = salt::sqlite::open(real.c_str());
  EXPECT_OK(d2r);
  salt::db d2 = std::move(*d2r);
  EXPECT_EQ(*d2.scalar<std::int64_t>("PRAGMA secure_delete"), 1);
  EXPECT_EQ(*d2.scalar<std::int64_t>("PRAGMA foreign_keys"), 1);
  EXPECT_EQ(*d2.scalar<std::int64_t>("PRAGMA trusted_schema"), 0);

  // WAL is opt-in.
  auto wal = salt::sqlite::open((dir / "wal.db").c_str(), {.wal = true});
  EXPECT_OK(wal);
  EXPECT_EQ(*wal->scalar<std::string>("PRAGMA journal_mode"), "wal");

  fs::remove_all(dir, ec);
}


// --- composite primary keys --------------------------------------------------

struct [[=salt::table("tenant_things")]] TenantThing {
  [[=salt::pk{}]] std::string tenant_id;
  [[=salt::pk{}]] std::string thing_id;
  std::string status;
  std::optional<std::string> note;
};

static void test_composite_pk() {
  static_assert(salt::detail::pk_count<TenantThing>() == 2);

  // One pk member is a column constraint; several are one table constraint.
  EXPECT_EQ(salt::create_table_sql<TenantThing>(salt::sqlite_dialect),
            "CREATE TABLE IF NOT EXISTS \"tenant_things\" ("
            "\"tenant_id\" TEXT NOT NULL, "
            "\"thing_id\" TEXT NOT NULL, "
            "\"status\" TEXT NOT NULL, "
            "\"note\" TEXT, "
            "PRIMARY KEY (\"tenant_id\", \"thing_id\"))");
  EXPECT_EQ(salt::create_table_sql<TenantThing>(salt::postgres_dialect),
            "CREATE TABLE IF NOT EXISTS \"tenant_things\" ("
            "\"tenant_id\" TEXT NOT NULL, "
            "\"thing_id\" TEXT NOT NULL, "
            "\"status\" TEXT NOT NULL, "
            "\"note\" TEXT, "
            "PRIMARY KEY (\"tenant_id\", \"thing_id\"))");
  EXPECT(salt::create_table_sql<TenantThing>(salt::mariadb_dialect)
             .contains("PRIMARY KEY (`tenant_id`, `thing_id`)"));

  // The composite key is real: the second insert of the same pair conflicts,
  // the same id under another tenant does not.
  auto dbr = salt::sqlite::open_memory();
  EXPECT_OK(dbr);
  if (!dbr) return;
  salt::db db = std::move(*dbr);
  EXPECT_OK(db.create_table<TenantThing>());
  EXPECT_OK(db.insert(TenantThing{"acme", "t-1", "active", {}}));
  EXPECT_OK(db.insert(TenantThing{"umbrella", "t-1", "active", {}}));
  auto dup2 = db.insert(TenantThing{"acme", "t-1", "again", {}});
  EXPECT(!dup2 && dup2.error().code == salt::errc::constraint);

  auto row = db.query_one<TenantThing>(
      "WHERE tenant_id = ? AND thing_id = ?", std::string("acme"),
      std::string("t-1"));
  EXPECT_OK(row);
  EXPECT(row && *row && (**row).status == "active");
}

// --- execute(): statement-scoped rows-affected --------------------------------

static void test_execute_affected() {
  auto dbr = salt::sqlite::open_memory();
  EXPECT_OK(dbr);
  if (!dbr) return;
  salt::db db = std::move(*dbr);
  EXPECT_OK(db.exec("CREATE TABLE t (x INTEGER, y TEXT)"));
  EXPECT_OK(db.exec("INSERT INTO t VALUES (1,'a'), (2,'b'), (3,'c')"));

  EXPECT_EQ(db.execute("UPDATE t SET y = ? WHERE x < ?", std::string("z"),
                       std::int64_t{3})
                .value_or(-1),
            std::int64_t{2});
  EXPECT_EQ(db.execute("UPDATE t SET y = ? WHERE x = ?", std::string("z"),
                       std::int64_t{99})
                .value_or(-1),
            std::int64_t{0});
  EXPECT_EQ(db.execute("DELETE FROM t WHERE x >= ?", std::int64_t{2})
                .value_or(-1),
            std::int64_t{2});
  // A second statement sneaking in through execute is refused like anywhere
  // else that prepares.
  EXPECT(!db.execute("DELETE FROM t; DELETE FROM t"));
}

// --- migrate concurrency seam --------------------------------------------------

static void test_migrate_lock_seam() {
  auto dbr = salt::sqlite::open_memory();
  EXPECT_OK(dbr);
  if (!dbr) return;
  salt::db db = std::move(*dbr);

  // Lock before work, unlock after — and unlock even when the run fails.
  int events = 0;                 // 1 = locked, 2 = unlocked (order encoded)
  int locked_at = 0, unlocked_at = 0;
  salt::migrate_options o;
  o.lock = [&](salt::db&) -> salt::result<void> {
    locked_at = ++events;
    return {};
  };
  o.unlock = [&](salt::db&) { unlocked_at = ++events; };

  auto ok = salt::migrate(db, kV1, o);
  EXPECT_OK(ok);
  EXPECT_EQ(locked_at, 1);
  EXPECT_EQ(unlocked_at, 2);

  // A failing migration still unlocks.
  static constexpr salt::migration kBad[] = {
      {1, "v1", "CREATE TABLE m1 (x INTEGER)", "DROP TABLE m1"},
      {2, "boom", "INSERT INTO does_not_exist VALUES (1)", ""},
  };
  events = 0;
  locked_at = unlocked_at = 0;
  auto bad = salt::migrate(db, kBad, o);
  EXPECT(!bad);
  EXPECT_EQ(locked_at, 1);
  EXPECT_EQ(unlocked_at, 2);

  // A refused lock stops the run before any history is touched.
  auto dbr2 = salt::sqlite::open_memory();
  EXPECT_OK(dbr2);
  if (!dbr2) return;
  salt::db db2 = std::move(*dbr2);
  salt::migrate_options refuse;
  refuse.lock = [&](salt::db&) -> salt::result<void> {
    return salt::fail(salt::errc::exec, "lock refused on purpose");
  };
  bool unlocked = false;
  refuse.unlock = [&](salt::db&) { unlocked = true; };
  EXPECT(!salt::migrate(db2, kV1, refuse));
  EXPECT(!unlocked);
  EXPECT_EQ(db2.count<salt::schema_history>().value_or(-1), std::int64_t{-1});
}


// --- table-level constraints ---------------------------------------------------

struct [[=salt::table("tt_children")]]
       [[=salt::table_constraint(
           "FOREIGN KEY (tenant_id, parent_id) "
           "REFERENCES tenant_things(tenant_id, thing_id)")]]
       [[=salt::table_constraint("UNIQUE (tenant_id, label)")]] TtChild {
  [[=salt::pk{}]] std::string tenant_id;
  [[=salt::pk{}]] std::string child_id;
  std::string parent_id;
  std::string label;
};

static void test_table_constraints() {
  EXPECT_EQ(salt::create_table_sql<TtChild>(salt::sqlite_dialect),
            "CREATE TABLE IF NOT EXISTS \"tt_children\" ("
            "\"tenant_id\" TEXT NOT NULL, "
            "\"child_id\" TEXT NOT NULL, "
            "\"parent_id\" TEXT NOT NULL, "
            "\"label\" TEXT NOT NULL, "
            "PRIMARY KEY (\"tenant_id\", \"child_id\"), "
            "FOREIGN KEY (tenant_id, parent_id) "
            "REFERENCES tenant_things(tenant_id, thing_id), "
            "UNIQUE (tenant_id, label))");

  // And they are real: the composite FK refuses an orphan, the multi-column
  // UNIQUE refuses a duplicate pair (sqlite enforces both — foreign_keys=ON
  // is part of open()).
  auto dbr = salt::sqlite::open_memory();
  EXPECT_OK(dbr);
  if (!dbr) return;
  salt::db db = std::move(*dbr);
  EXPECT_OK(db.create_table<TenantThing>());
  EXPECT_OK(db.create_table<TtChild>());
  EXPECT_OK(db.insert(TenantThing{"acme", "t-1", "active", {}}));
  EXPECT_OK(db.insert(TtChild{"acme", "c-1", "t-1", "one"}));
  auto orphan = db.insert(TtChild{"acme", "c-2", "ghost", "two"});
  EXPECT(!orphan && orphan.error().code == salt::errc::constraint);
  auto duplab = db.insert(TtChild{"acme", "c-3", "t-1", "one"});
  EXPECT(!duplab && duplab.error().code == salt::errc::constraint);
}

int main() {
  test_model();
  test_tokenizer();
  test_crud();
  test_sql_hygiene();
  test_integer_and_time_columns();
  test_sensitive_redaction();
  test_sardine_columns();
  test_transactions();
  test_migrations();
  test_changelog_control();
  test_sqlite_hardening();
  test_composite_pk();
  test_table_constraints();
  test_execute_affected();
  test_migrate_lock_seam();
  failures += salt::conformance::run([] {
    auto d = salt::sqlite::open_memory();
    return d ? std::move(*d) : salt::db{};
  });
  if (failures == 0) std::println("all tests passed");
  else std::println("{} FAILURES", failures);
  return failures != 0;
}
