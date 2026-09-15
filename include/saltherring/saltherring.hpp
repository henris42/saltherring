// saltherring — SQL persistence for C++26: tables, queries and migrations for
// any struct, out of the box. One header, no macros, no codegen — reflection
// for the object↔row mapping, annotations for the schema, contracts on the
// API. Value encoding is shared with sardine: anything sardine can serialize
// can live in a column (structs, vectors, maps, variants → JSON TEXT; enums →
// their sardine wire name; uint8_t sequences → BLOB).
//
// Core is driver-agnostic. Drivers: <saltherring/sqlite.hpp> (tested),
// <saltherring/pg.hpp> and <saltherring/mariadb.hpp> (compiled where the
// client headers exist). SQL dialects for all three live here and are pure
// functions of the model — testable without a server.
//
// Requires GCC 16.1, -std=c++26 -freflection -fcontracts, and sardine on the
// include path.

#ifndef SALTHERRING_HPP
#define SALTHERRING_HPP

#include <sardine/sardine.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace salt {

// ---------------------------------------------------------------------------
// Annotations.
//
// Same rules as sardine: annotation objects must be structural, so names are
// stored by value (sardine::detail::fixed_string). GCC 16.1 quirk applies
// here too: [[=salt::column{"x"}]] does not parse — use salt::column("x").
// ---------------------------------------------------------------------------

namespace detail {
  using sardine::detail::fixed_string;
}

// [[=salt::table("users")]] — on a struct: the table name. Default: the type
// identifier converted PascalCase → snake_case (User → user).
struct table : detail::fixed_string {
  using fixed_string::fixed_string;
};

// [[=salt::column("created")]] — the column name. Default: the member
// identifier, verbatim. Independent of sardine::rename on purpose: the wire
// name and the column name are different contracts.
struct column : detail::fixed_string {
  using fixed_string::fixed_string;
};

// [[=salt::pk{}]] — primary key. Exactly one per persisted struct for the
// by-id operations (find/update/erase). [[=salt::auto_pk]] is a primary key
// the database assigns (AUTOINCREMENT / BIGSERIAL / AUTO_INCREMENT); it must
// be a non-optional integer member, insert() skips it and writes the
// assigned id back.
struct pk {
  bool auto_increment = false;
};
inline constexpr pk auto_pk{true};

// [[=salt::transient{}]] — not persisted; invisible to DDL, reads and writes.
struct transient {};

// [[=salt::unique{}]] — UNIQUE column constraint.
struct unique {};

// [[=salt::indexed{}]] — CREATE INDEX idx_<table>_<column> alongside the table.
struct indexed {};

// [[=salt::check("balance >= 0")]] — CHECK constraint, expression verbatim.
struct check : detail::fixed_string {
  using fixed_string::fixed_string;
};

// [[=salt::sql_default("0")]] — DEFAULT clause, expression verbatim
// (quote string literals yourself: sql_default("'pending'")).
struct sql_default : detail::fixed_string {
  using fixed_string::fixed_string;
};

// [[=salt::references("users")]] / [[=salt::references("users", "id")]] —
// FOREIGN KEY to another table. Column defaults to "id".
struct references {
  detail::fixed_string ref_table;
  detail::fixed_string ref_column;
  consteval references(const char* t, const char* c = "id")
      : ref_table(t), ref_column(c) {}
};

// ---------------------------------------------------------------------------
// Errors. Same shape as sardine: branch on the code, read the message.
// ---------------------------------------------------------------------------

enum class errc : std::uint8_t {
  closed,              // operation on a default-constructed / moved-from db
  connect,             // could not open the database
  prepare,             // SQL did not compile
  bind,                // parameter could not be bound
  exec,                // statement execution failed (constraints land here)
  type_mismatch,       // column value has the wrong runtime type
  out_of_range,        // integer does not fit the member type
  unknown_enum,        // TEXT value is not an enumerator name
  serialization,       // sardine to_json/from_json of a JSON column failed
  no_rows,             // scalar query produced no row
  migration_duplicate, // two migrations share a version
  migration_checksum,  // an applied migration's SQL changed after the fact
  migration_missing,   // history has a version the migration list lacks
  migration_order,     // a pending migration is older than an applied one
  migration_no_down,   // unwind requested but no down SQL was recorded
};

struct error {
  std::string message;
  errc code = errc::exec;
  std::string sql;  // the statement involved, when there is one
};

template <typename T>
using result = std::expected<T, error>;

inline std::unexpected<error> fail(errc c, std::string msg, std::string sql = {}) {
  return std::unexpected(error{std::move(msg), c, std::move(sql)});
}

// ---------------------------------------------------------------------------
// Values on the wire between the mapper and a driver. Five shapes, exactly
// the SQL storage classes every backend shares. Drivers normalize into these
// (Postgres BOOLEAN → integer 0/1, BYTEA → blob, ...).
// ---------------------------------------------------------------------------

struct sql_null {};
using sql_value =
    std::variant<sql_null, std::int64_t, double, std::string, std::vector<std::uint8_t>>;

// ---------------------------------------------------------------------------
// Dialects. The SQL flavor differences, as data. Generation is a pure
// function of (reflected model, dialect) — the Postgres and MariaDB texts
// are unit-testable with no server in sight.
// ---------------------------------------------------------------------------

enum class placeholder_style : std::uint8_t {
  question,  // ?      (SQLite, MariaDB)
  dollar_n,  // $1 $2  (Postgres)
};

struct dialect {
  std::string_view name;
  placeholder_style placeholders;
  char quote;                       // identifier quote: " (SQL) or ` (MariaDB)
  std::string_view type_integer;
  std::string_view type_real;
  std::string_view type_text;
  std::string_view type_text_key;   // TEXT usable in PK/UNIQUE/INDEX; empty =
                                    // type_text is fine (MariaDB needs VARCHAR)
  std::string_view type_blob;
  std::string_view type_bool;
  std::string_view auto_pk_column;  // full "type + constraint" for auto_pk
  bool insert_returning;            // append RETURNING <pk> and read the id
                                    // from the row instead of last_insert_id
};

inline constexpr dialect sqlite_dialect{
    "sqlite", placeholder_style::question, '"',
    "INTEGER", "REAL", "TEXT", {}, "BLOB", "INTEGER",
    "INTEGER PRIMARY KEY AUTOINCREMENT", false};

inline constexpr dialect postgres_dialect{
    "postgresql", placeholder_style::dollar_n, '"',
    "BIGINT", "DOUBLE PRECISION", "TEXT", {}, "BYTEA", "BOOLEAN",
    "BIGSERIAL PRIMARY KEY", true};

inline constexpr dialect mariadb_dialect{
    "mariadb", placeholder_style::question, '`',
    "BIGINT", "DOUBLE", "TEXT", "VARCHAR(255)", "BLOB", "BOOLEAN",
    "BIGINT PRIMARY KEY AUTO_INCREMENT", false};

// ---------------------------------------------------------------------------
// The reflected model: which members persist, under what column name, as
// which SQL shape. All consteval; names land in static storage.
// ---------------------------------------------------------------------------

enum class col_kind : std::uint8_t {
  boolean, integer, real, text, blob,
  enum_text,  // TEXT holding the sardine wire name of the enumerator
  json,       // TEXT holding sardine JSON — any other sardine-serializable type
};

struct column_info {
  std::string_view name;
  col_kind kind = col_kind::text;
  bool nullable = false;   // member was std::optional
  bool is_pk = false;
  bool auto_inc = false;
  bool is_unique = false;
  bool is_indexed = false;
  std::string_view check_expr;    // empty = none
  std::string_view default_expr;  // empty = none
  std::string_view ref_table;     // empty = no foreign key
  std::string_view ref_column;
};

namespace detail {

using sardine::detail::annotation_of;
using sardine::detail::has;
using sardine::detail::members_of;
using sardine::detail::enumerators_of;
using sardine::detail::is_optional;
using sardine::detail::string_like;
using sardine::detail::byte_sequence;
using sardine::detail::json_name;
using sardine::detail::type_name;

template <typename T> struct unwrap_optional { using type = T; };
template <typename T> struct unwrap_optional<std::optional<T>> { using type = T; };
template <typename T> using unwrap_optional_t = unwrap_optional<T>::type;

consteval std::string_view pascal_to_snake(std::string_view id) {
  std::string out;
  for (std::size_t i = 0; i < id.size(); ++i) {
    char c = id[i];
    if (c >= 'A' && c <= 'Z') {
      if (i != 0) out += '_';
      out += char(c - 'A' + 'a');
    } else {
      out += c;
    }
  }
  return std::define_static_string(out);
}

template <typename T>
consteval std::string_view table_name() {
  if (auto t = annotation_of<table>(^^T))
    return std::define_static_string(t->str());
  return pascal_to_snake(std::meta::identifier_of(^^T));
}

consteval std::string_view column_name(std::meta::info m) {
  if (auto c = annotation_of<column>(m))
    return std::define_static_string(c->str());
  return std::meta::identifier_of(m);
}

template <typename M>
consteval col_kind kind_of() {
  using U = unwrap_optional_t<M>;
  if constexpr (std::same_as<U, bool>) return col_kind::boolean;
  else if constexpr (std::is_enum_v<U>) return col_kind::enum_text;
  else if constexpr (std::integral<U>) return col_kind::integer;
  else if constexpr (std::floating_point<U>) return col_kind::real;
  else if constexpr (byte_sequence<U>) return col_kind::blob;
  else if constexpr (string_like<U>) return col_kind::text;
  else return col_kind::json;  // sardine takes it from here (or rejects it
                               // with its own static_assert)
}

consteval bool persisted(std::meta::info m) { return !has<transient>(m); }

template <typename T>
consteval std::size_t column_count() {
  std::size_t n = 0;
  template for (constexpr auto m : members_of<T>())
    if constexpr (persisted(m)) ++n;
  return n;
}

template <typename T>
consteval auto build_columns() {
  std::array<column_info, column_count<T>()> out{};
  std::size_t i = 0;
  template for (constexpr auto m : members_of<T>()) {
    if constexpr (persisted(m)) {
      using M = [:std::meta::type_of(m):];
      column_info c{};
      c.name = column_name(m);
      c.kind = kind_of<M>();
      c.nullable = is_optional<M>::value;
      if (auto p = annotation_of<pk>(m)) {
        c.is_pk = true;
        c.auto_inc = p->auto_increment;
      }
      c.is_unique = has<unique>(m);
      c.is_indexed = has<indexed>(m);
      if (auto ch = annotation_of<check>(m))
        c.check_expr = std::define_static_string(ch->str());
      if (auto df = annotation_of<sql_default>(m))
        c.default_expr = std::define_static_string(df->str());
      if (auto r = annotation_of<references>(m)) {
        c.ref_table = std::define_static_string(r->ref_table.str());
        c.ref_column = std::define_static_string(r->ref_column.str());
      }
      if (c.auto_inc && (c.kind != col_kind::integer || c.nullable))
        throw "saltherring: auto_pk must be a non-optional integer member";
      if (c.is_pk && c.nullable)
        throw "saltherring: a primary key cannot be std::optional";
      out[i++] = c;
    }
  }
  return out;
}

template <typename T>
inline constexpr auto columns = build_columns<T>();

template <typename T>
consteval std::size_t pk_count() {
  std::size_t n = 0;
  for (const column_info& c : columns<T>) n += c.is_pk;
  return n;
}

template <typename T>
consteval column_info pk_column() {
  for (const column_info& c : columns<T>)
    if (c.is_pk) return c;
  throw "saltherring: type has no [[=salt::pk{}]] member";
}

template <typename T>
concept entity = sardine::detail::reflectable_struct<T> && column_count<T>() > 0;

template <typename T>
concept keyed_entity = entity<T> && pk_count<T>() == 1;

// Visit the pk member of an object: f(member_lvalue).
template <typename T, typename F>
constexpr void with_pk(T&& obj, F&& f) {
  template for (constexpr auto m : members_of<std::remove_cvref_t<T>>()) {
    if constexpr (persisted(m) && has<pk>(m)) f(obj.[:m:]);
  }
}

}  // namespace detail

// The reflected columns of T, for inspection and tests.
template <detail::entity T>
constexpr std::span<const column_info> columns_of() {
  return detail::columns<T>;
}

template <detail::entity T>
consteval std::string_view table_of() {
  return detail::table_name<T>();
}

// ---------------------------------------------------------------------------
// member value ↔ sql_value.
//
// This is the sardine compatibility line: enums store their sardine wire
// name (rename / rename_all on the enum are honored, enum_from_number lets
// TEXT-or-INTEGER reads through), byte sequences store as BLOB exactly where
// sardine's CBOR would emit major 2, and everything sardine serializes but
// SQL cannot store natively becomes sardine JSON in a TEXT column.
// ---------------------------------------------------------------------------

namespace detail {

template <typename E>
  requires std::is_enum_v<E>
std::string enum_text(E v) {
  template for (constexpr auto e : enumerators_of<E>()) {
    constexpr std::string_view n = json_name<E>(e);
    if (v == [:e:]) return std::string(n);
  }
  // Outside the named enumerators: decimal underlying, like sardine's JSON
  // writer degrades to a number. Reads it back only under enum_from_number.
  return std::to_string(std::to_underlying(v));
}

template <typename E>
  requires std::is_enum_v<E>
result<E> enum_from_text(std::string_view s) {
  template for (constexpr auto e : enumerators_of<E>()) {
    constexpr std::string_view n = json_name<E>(e);
    if (s == n) return [:e:];
  }
  if constexpr (has<sardine::enum_from_number>(^^E)) {
    std::underlying_type_t<E> raw{};
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), raw);
    if (ec == std::errc{} && p == s.data() + s.size()) return static_cast<E>(raw);
  }
  return fail(errc::unknown_enum,
              std::format("'{}' is not an enumerator of {}", s, type_name<E>()));
}

template <typename M>
result<sql_value> to_sql(const M& v) {
  if constexpr (is_optional<M>::value) {
    if (!v) return sql_value{sql_null{}};
    return to_sql(*v);
  } else if constexpr (std::same_as<M, bool>) {
    return sql_value{std::int64_t(v)};
  } else if constexpr (std::is_enum_v<M>) {
    return sql_value{enum_text(v)};
  } else if constexpr (std::integral<M>) {
    return sql_value{static_cast<std::int64_t>(v)};
  } else if constexpr (std::floating_point<M>) {
    return sql_value{static_cast<double>(v)};
  } else if constexpr (byte_sequence<M>) {
    return sql_value{std::vector<std::uint8_t>(std::ranges::begin(v), std::ranges::end(v))};
  } else if constexpr (string_like<M>) {
    return sql_value{std::string(std::string_view(v))};
  } else {
    return sql_value{sardine::to_json(v)};
  }
}

template <typename M>
result<void> from_sql(const sql_value& v, M& out) {
  if constexpr (is_optional<M>::value) {
    if (std::holds_alternative<sql_null>(v)) {
      out = std::nullopt;
      return {};
    }
    return from_sql(v, out.emplace());
  } else {
    if (std::holds_alternative<sql_null>(v))
      return fail(errc::type_mismatch, "NULL in a non-optional member");
    if constexpr (std::same_as<M, bool>) {
      if (auto* i = std::get_if<std::int64_t>(&v)) {
        out = *i != 0;
        return {};
      }
      return fail(errc::type_mismatch, "expected an integer for bool");
    } else if constexpr (std::is_enum_v<M>) {
      if (auto* s = std::get_if<std::string>(&v)) {
        auto e = enum_from_text<M>(*s);
        if (!e) return std::unexpected(e.error());
        out = *e;
        return {};
      }
      if constexpr (has<sardine::enum_from_number>(^^M)) {
        if (auto* i = std::get_if<std::int64_t>(&v)) {
          out = static_cast<M>(*i);
          return {};
        }
      }
      return fail(errc::type_mismatch, "expected TEXT for an enum column");
    } else if constexpr (std::integral<M>) {
      std::int64_t i;
      if (auto* p = std::get_if<std::int64_t>(&v)) {
        i = *p;
      } else if (auto* d = std::get_if<double>(&v);
                 d && *d == static_cast<double>(static_cast<std::int64_t>(*d))) {
        i = static_cast<std::int64_t>(*d);  // integral REAL: some drivers do this
      } else {
        return fail(errc::type_mismatch, "expected an integer");
      }
      if (!std::in_range<M>(i))
        return fail(errc::out_of_range,
                    std::format("{} does not fit the member type", i));
      out = static_cast<M>(i);
      return {};
    } else if constexpr (std::floating_point<M>) {
      if (auto* d = std::get_if<double>(&v)) {
        out = static_cast<M>(*d);
        return {};
      }
      if (auto* i = std::get_if<std::int64_t>(&v)) {
        out = static_cast<M>(*i);
        return {};
      }
      return fail(errc::type_mismatch, "expected a number");
    } else if constexpr (byte_sequence<M>) {
      if (auto* b = std::get_if<std::vector<std::uint8_t>>(&v)) {
        out = M(b->begin(), b->end());
        return {};
      }
      return fail(errc::type_mismatch, "expected a BLOB");
    } else if constexpr (string_like<M>) {
      if (auto* s = std::get_if<std::string>(&v)) {
        out = M(*s);
        return {};
      }
      return fail(errc::type_mismatch, "expected TEXT");
    } else {
      if (auto* s = std::get_if<std::string>(&v)) {
        auto r = sardine::from_json<M>(*s);
        if (!r)
          return fail(errc::serialization,
                      std::format("JSON column: {} at '{}'", r.error().message,
                                  r.error().path));
        out = std::move(*r);
        return {};
      }
      return fail(errc::type_mismatch, "expected TEXT holding JSON");
    }
  }
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Driver interface. A backend implements two small classes over sql_value;
// everything reflective stays above this line. Contracts guard the calling
// conventions at the salt::db layer, not on the virtuals.
// ---------------------------------------------------------------------------

namespace backend {

struct statement {
  virtual ~statement() = default;
  virtual result<void> bind(int index, const sql_value& v) = 0;  // 1-based
  virtual result<bool> step() = 0;             // true = a row is available
  virtual result<sql_value> column(int index) = 0;  // 0-based, within a row
  virtual int column_count() = 0;
};

struct connection {
  virtual ~connection() = default;
  virtual result<std::unique_ptr<statement>> prepare(std::string_view sql) = 0;
  virtual result<void> exec(std::string_view sql) = 0;  // multi-statement OK
  virtual result<std::int64_t> last_insert_id() = 0;
  virtual const dialect& dial() const = 0;
};

}  // namespace backend

// ---------------------------------------------------------------------------
// SQL generation. Pure text from (model, dialect).
// ---------------------------------------------------------------------------

namespace detail {

inline void quoted(std::string& out, std::string_view id, const dialect& d) {
  out += d.quote;
  out += id;
  out += d.quote;
}

inline void placeholder(std::string& out, const dialect& d, int n) {
  if (d.placeholders == placeholder_style::dollar_n) {
    out += '$';
    out += std::to_string(n);
  } else {
    out += '?';
  }
}

// Rewrite user-facing '?' placeholders into the dialect's style, skipping
// 'string literals'. first is the number the first ? becomes.
inline std::string adapt_placeholders(std::string_view sql, const dialect& d,
                                      int first = 1) {
  if (d.placeholders == placeholder_style::question) return std::string(sql);
  std::string out;
  out.reserve(sql.size() + 8);
  bool in_string = false;
  int n = first;
  for (char c : sql) {
    if (c == '\'') in_string = !in_string;
    if (c == '?' && !in_string) placeholder(out, d, n++);
    else out += c;
  }
  return out;
}

inline std::string_view sql_type(const column_info& c, const dialect& d) {
  bool keyish = c.is_pk || c.is_unique || c.is_indexed;
  switch (c.kind) {
    case col_kind::boolean: return d.type_bool;
    case col_kind::integer: return d.type_integer;
    case col_kind::real:    return d.type_real;
    case col_kind::blob:    return d.type_blob;
    default:
      return (keyish && !d.type_text_key.empty()) ? d.type_text_key : d.type_text;
  }
}

}  // namespace detail

// CREATE TABLE IF NOT EXISTS (plus CREATE INDEX statements for [[=salt::indexed{}]]
// columns), ';'-joined. What db::create_table<T>() executes; also handy as the
// starting text of a migration.
template <detail::entity T>
std::string create_table_sql(const dialect& d) {
  constexpr std::string_view tname = table_of<T>();
  std::string out = "CREATE TABLE IF NOT EXISTS ";
  detail::quoted(out, tname, d);
  out += " (";
  bool first = true;
  for (const column_info& c : detail::columns<T>) {
    if (!std::exchange(first, false)) out += ", ";
    detail::quoted(out, c.name, d);
    out += ' ';
    if (c.auto_inc) {
      out += d.auto_pk_column;
    } else {
      out += detail::sql_type(c, d);
      if (c.is_pk) out += " PRIMARY KEY";
      else if (!c.nullable) out += " NOT NULL";
    }
    if (c.is_unique) out += " UNIQUE";
    if (!c.default_expr.empty()) {
      out += " DEFAULT ";
      out += c.default_expr;
    }
    if (!c.check_expr.empty()) {
      out += " CHECK (";
      out += c.check_expr;
      out += ')';
    }
    if (!c.ref_table.empty()) {
      out += " REFERENCES ";
      detail::quoted(out, c.ref_table, d);
      out += '(';
      detail::quoted(out, c.ref_column, d);
      out += ')';
    }
  }
  out += ')';
  for (const column_info& c : detail::columns<T>) {
    if (!c.is_indexed) continue;
    out += ";\nCREATE INDEX IF NOT EXISTS ";
    std::string idx = std::format("idx_{}_{}", tname, c.name);
    detail::quoted(out, idx, d);
    out += " ON ";
    detail::quoted(out, tname, d);
    out += " (";
    detail::quoted(out, c.name, d);
    out += ')';
  }
  return out;
}

namespace detail {

// SELECT <all columns> FROM <table> — columns are always listed explicitly,
// in member order; row decoding relies on that order.
template <entity T>
std::string select_sql(const dialect& d) {
  constexpr std::string_view tname = table_of<T>();
  std::string out = "SELECT ";
  bool first = true;
  for (const column_info& c : columns<T>) {
    if (!std::exchange(first, false)) out += ", ";
    quoted(out, c.name, d);
  }
  out += " FROM ";
  quoted(out, tname, d);
  return out;
}

template <entity T>
std::string insert_sql(const dialect& d) {
  constexpr std::string_view tname = table_of<T>();
  std::string out = "INSERT INTO ";
  quoted(out, tname, d);
  out += " (";
  bool first = true;
  for (const column_info& c : columns<T>) {
    if (c.auto_inc) continue;
    if (!std::exchange(first, false)) out += ", ";
    quoted(out, c.name, d);
  }
  out += ") VALUES (";
  first = true;
  int n = 1;
  for (const column_info& c : columns<T>) {
    if (c.auto_inc) continue;
    if (!std::exchange(first, false)) out += ", ";
    placeholder(out, d, n++);
  }
  out += ')';
  if constexpr (pk_count<T>() == 1) {
    constexpr column_info pkc = pk_column<T>();
    if (d.insert_returning && pkc.auto_inc) {
      out += " RETURNING ";
      quoted(out, pkc.name, d);
    }
  }
  return out;
}

template <keyed_entity T>
std::string update_sql(const dialect& d) {
  constexpr std::string_view tname = table_of<T>();
  constexpr column_info pkc = pk_column<T>();
  std::string out = "UPDATE ";
  quoted(out, tname, d);
  out += " SET ";
  bool first = true;
  int n = 1;
  for (const column_info& c : columns<T>) {
    if (c.is_pk) continue;
    if (!std::exchange(first, false)) out += ", ";
    quoted(out, c.name, d);
    out += " = ";
    placeholder(out, d, n++);
  }
  out += " WHERE ";
  quoted(out, pkc.name, d);
  out += " = ";
  placeholder(out, d, n);
  return out;
}

template <keyed_entity T>
std::string delete_sql(const dialect& d) {
  constexpr std::string_view tname = table_of<T>();
  constexpr column_info pkc = pk_column<T>();
  std::string out = "DELETE FROM ";
  quoted(out, tname, d);
  out += " WHERE ";
  quoted(out, pkc.name, d);
  out += " = ";
  placeholder(out, d, 1);
  return out;
}

}  // namespace detail

// ---------------------------------------------------------------------------
// db — a connection plus the object mapper. Movable, not copyable. All
// operations require an open connection (precondition; also reported as
// errc::closed when contracts are compiled out).
// ---------------------------------------------------------------------------

class db {
 public:
  db() = default;
  explicit db(std::unique_ptr<backend::connection> c) : c_(std::move(c)) {}

  bool open() const { return c_ != nullptr; }
  const dialect& dial() const pre(open()) { return c_->dial(); }

  // Raw SQL with ?-placeholders (adapted to the dialect) and typed binds.
  // (contract_assert, not pre: GCC 16.1 ICEs on pre() when a variadic member
  // template is instantiated with an empty pack.)
  template <typename... Args>
  result<void> exec(std::string_view sql, const Args&... args) {
    contract_assert(open());
    if (!open()) return fail(errc::closed, "db is not open");
    if constexpr (sizeof...(Args) == 0) {
      return c_->exec(detail::adapt_placeholders(sql, dial()));
    } else {
      auto st = prepare_bound(detail::adapt_placeholders(sql, dial()), args...);
      if (!st) return std::unexpected(st.error());
      return (*st)->step().transform([](bool) {});
    }
  }

  // One value from a one-row query: db.scalar<std::int64_t>("SELECT COUNT(*)...").
  template <typename V, typename... Args>
  result<V> scalar(std::string_view sql, const Args&... args) {
    contract_assert(open());
    if (!open()) return fail(errc::closed, "db is not open");
    auto st = prepare_bound(detail::adapt_placeholders(sql, dial()), args...);
    if (!st) return std::unexpected(st.error());
    auto row = (*st)->step();
    if (!row) return std::unexpected(row.error());
    if (!*row) return fail(errc::no_rows, "query produced no rows", std::string(sql));
    auto v = (*st)->column(0);
    if (!v) return std::unexpected(v.error());
    V out{};
    return detail::from_sql(*v, out).transform([&] { return std::move(out); });
  }

  template <detail::entity T>
  result<void> create_table() {
    contract_assert(open());
    if (!open()) return fail(errc::closed, "db is not open");
    return c_->exec(create_table_sql<T>(dial()));
  }

  template <detail::entity... Ts>
  result<void> create_tables() {
    contract_assert(open());
    result<void> r{};
    ((r = r ? create_table<Ts>() : r), ...);
    return r;
  }

  // INSERT. Returns the primary key when the model has a single-column pk
  // (the database-assigned id for auto_pk, and writes it back into obj when
  // obj is non-const), 0 otherwise.
  template <detail::entity T>
  result<std::int64_t> insert(T& obj) {
    contract_assert(open());
    auto id = insert_impl(std::as_const(obj));
    if constexpr (detail::pk_count<T>() == 1) {
      constexpr column_info pkc = detail::pk_column<T>();
      if constexpr (pkc.auto_inc) {
        if (id)
          detail::with_pk(obj, [&](auto& m) {
            using PM = std::remove_reference_t<decltype(m)>;
            if constexpr (std::integral<PM>) m = static_cast<PM>(*id);
          });
      }
    }
    return id;
  }
  template <detail::entity T>
  result<std::int64_t> insert(const T& obj) {
    contract_assert(open());
    return insert_impl(obj);
  }

  // SELECT by primary key. Absence is not an error: expected(nullopt).
  template <detail::keyed_entity T, typename K>
  result<std::optional<T>> find(const K& key) {
    contract_assert(open());
    if (!open()) return fail(errc::closed, "db is not open");
    constexpr column_info pkc = detail::pk_column<T>();
    std::string sql = detail::select_sql<T>(dial()) + " WHERE ";
    detail::quoted(sql, pkc.name, dial());
    sql += " = ";
    detail::placeholder(sql, dial(), 1);
    auto rows = fetch<T>(sql, key);
    if (!rows) return std::unexpected(rows.error());
    if (rows->empty()) return std::optional<T>{};
    return std::optional<T>{std::move(rows->front())};
  }

  // SELECT with a tail you write: db.query<User>("WHERE balance > ? ORDER BY
  // name", 10.0). The column list and table come from the model.
  template <detail::entity T, typename... Args>
  result<std::vector<T>> query(std::string_view tail = "", const Args&... args) {
    contract_assert(open());
    if (!open()) return fail(errc::closed, "db is not open");
    std::string sql = detail::select_sql<T>(dial());
    if (!tail.empty()) {
      sql += ' ';
      sql += detail::adapt_placeholders(tail, dial());
    }
    return fetch<T>(sql, args...);
  }

  // First row of a query<T>, if any.
  template <detail::entity T, typename... Args>
  result<std::optional<T>> query_one(std::string_view tail = "", const Args&... args) {
    contract_assert(open());
    auto rows = query<T>(tail, args...);
    if (!rows) return std::unexpected(rows.error());
    if (rows->empty()) return std::optional<T>{};
    return std::optional<T>{std::move(rows->front())};
  }

  template <detail::keyed_entity T>
  result<void> update(const T& obj) {
    contract_assert(open());
    if (!open()) return fail(errc::closed, "db is not open");
    std::string sql = detail::update_sql<T>(dial());
    auto st = c_->prepare(sql);
    if (!st) return std::unexpected(st.error());
    int idx = 1;
    result<void> r{};
    // Non-pk columns first (SET order), then the pk (WHERE).
    template for (constexpr auto m : detail::members_of<T>()) {
      if constexpr (detail::persisted(m) && !detail::has<pk>(m)) {
        if (r) r = bind_member(**st, idx, obj.[:m:]);
      }
    }
    if (r)
      detail::with_pk(obj, [&](const auto& m) { r = bind_member(**st, idx, m); });
    if (!r) return r;
    return (*st)->step().transform([](bool) {});
  }

  template <detail::keyed_entity T, typename K>
  result<void> erase(const K& key) {
    contract_assert(open());
    if (!open()) return fail(errc::closed, "db is not open");
    auto st = prepare_bound(detail::delete_sql<T>(dial()), key);
    if (!st) return std::unexpected(st.error());
    return (*st)->step().transform([](bool) {});
  }

  template <detail::entity T, typename... Args>
  result<std::int64_t> count(std::string_view tail = "", const Args&... args) {
    contract_assert(open());
    constexpr std::string_view tname = table_of<T>();
    std::string sql = "SELECT COUNT(*) FROM ";
    detail::quoted(sql, tname, dial());
    if (!tail.empty()) {
      sql += ' ';
      sql += tail;
    }
    return scalar<std::int64_t>(sql, args...);
  }

  // f: () -> result<void>. BEGIN, run, COMMIT — ROLLBACK on error.
  template <typename F>
  result<void> transaction(F&& f) {
    contract_assert(open());
    if (auto b = exec("BEGIN"); !b) return b;
    result<void> r = std::forward<F>(f)();
    if (!r) {
      (void)exec("ROLLBACK");
      return r;
    }
    return exec("COMMIT");
  }

  backend::connection& raw() pre(open()) { return *c_; }

 private:
  template <typename M>
  static result<void> bind_member(backend::statement& st, int& idx, const M& v) {
    auto sv = detail::to_sql(v);
    if (!sv) return std::unexpected(sv.error());
    return st.bind(idx++, *sv);
  }

  template <typename... Args>
  result<std::unique_ptr<backend::statement>> prepare_bound(
      const std::string& sql, const Args&... args) {
    auto st = c_->prepare(sql);
    if (!st) return st;
    [[maybe_unused]] int idx = 1;
    result<void> r{};
    ((r = r ? bind_member(**st, idx, args) : r), ...);
    if (!r) return std::unexpected(r.error());
    return st;
  }

  template <detail::entity T>
  static result<T> decode_row(backend::statement& st) {
    T out{};
    int i = 0;
    result<void> r{};
    template for (constexpr auto m : detail::members_of<T>()) {
      if constexpr (detail::persisted(m)) {
        if (r) {
          auto v = st.column(i++);
          if (!v) {
            r = std::unexpected(v.error());
          } else if (r = detail::from_sql(*v, out.[:m:]); !r) {
            constexpr std::string_view cname = detail::column_name(m);
            constexpr std::string_view tname = table_of<T>();
            r.error().message += std::format(" (column '{}' of {})", cname, tname);
          }
        }
      }
    }
    if (!r) return std::unexpected(r.error());
    return out;
  }

  template <detail::entity T, typename... Args>
  result<std::vector<T>> fetch(const std::string& sql, const Args&... args) {
    auto st = prepare_bound(sql, args...);
    if (!st) return std::unexpected(st.error());
    constexpr int ncols = int(detail::column_count<T>());
    std::vector<T> rows;
    for (;;) {
      auto more = (*st)->step();
      if (!more) return std::unexpected(more.error());
      if (!*more) break;
      contract_assert((*st)->column_count() >= ncols);
      auto row = decode_row<T>(**st);
      if (!row) return std::unexpected(row.error());
      rows.push_back(std::move(*row));
    }
    return rows;
  }

  template <detail::entity T>
  result<std::int64_t> insert_impl(const T& obj) {
    if (!open()) return fail(errc::closed, "db is not open");
    std::string sql = detail::insert_sql<T>(dial());
    auto st = c_->prepare(sql);
    if (!st) return std::unexpected(st.error());
    int idx = 1;
    result<void> r{};
    template for (constexpr auto m : detail::members_of<T>()) {
      if constexpr (detail::persisted(m) && !(detail::has<pk>(m) &&
                    detail::annotation_of<pk>(m)->auto_increment)) {
        if (r) r = bind_member(**st, idx, obj.[:m:]);
      }
    }
    if (!r) return std::unexpected(r.error());
    auto row = (*st)->step();
    if (!row) return std::unexpected(row.error());
    if constexpr (detail::pk_count<T>() == 1) {
      constexpr auto pkc = detail::pk_column<T>();
      if (pkc.auto_inc) {
        if (dial().insert_returning) {
          if (!*row) return fail(errc::exec, "INSERT ... RETURNING gave no row", sql);
          auto v = (*st)->column(0);
          if (!v) return std::unexpected(v.error());
          std::int64_t id = 0;
          auto c = detail::from_sql(*v, id);
          if (!c) return std::unexpected(c.error());
          return id;
        }
        return c_->last_insert_id();
      }
      // Non-auto single pk: return it as an int when it is one.
      std::int64_t id = 0;
      detail::with_pk(obj, [&](const auto& m) {
        if constexpr (std::integral<std::remove_cvref_t<decltype(m)>>)
          id = static_cast<std::int64_t>(m);
      });
      return id;
    }
    return 0;
  }

  std::unique_ptr<backend::connection> c_;
};

// ---------------------------------------------------------------------------
// Migrations — version control for the schema, Flyway/Liquibase-shaped.
//
// A migration is (version, description, up SQL or code, optional down SQL).
// migrate() keeps a salt_schema_history table: each applied migration is
// recorded with a checksum of its up SQL and a copy of its down SQL. Every
// later run proves the changelog against history — editing an applied
// migration is an error, a pending version older than an applied one is an
// error, and history holding a version the binary no longer knows is an
// error unless {.unwind_missing = true} lets migrate() roll it back using
// the down SQL recorded at apply time — a new deployment can unwind the old
// one's changelog without carrying its code. validate() runs the same proof
// and changes nothing; rollback() unwinds to a target version on demand.
// Each step runs in its own transaction (MariaDB DDL self-commits, exactly
// as in the Java tools).
// ---------------------------------------------------------------------------

struct migration {
  std::int64_t version = 0;
  std::string_view description;
  std::string_view sql;                 // up, used when fn is null
  std::string_view down;                // optional unwind SQL; recorded in history
  result<void> (*fn)(db&) = nullptr;    // up as code instead of SQL
};

// The history table, itself just a persisted struct.
struct [[=table("salt_schema_history")]] schema_history {
  [[=pk{}]] std::int64_t version = 0;
  std::string description;
  std::int64_t checksum = 0;
  std::string applied_at;
  std::optional<std::string> down_sql;  // how to unwind, exactly as applied
};

struct migrate_options {
  // When history holds versions the migration list no longer has, unwind
  // them (newest first) with their recorded down SQL instead of failing.
  bool unwind_missing = false;
};

struct migrate_report {
  int applied = 0;    // run by this call
  int validated = 0;  // already in history, checksums verified
  int unwound = 0;    // rolled back by this call (unwind_missing)
};

struct validate_report {
  int validated = 0;  // applied and provably unchanged
  int pending = 0;    // in the list, not yet applied
};

namespace detail {

// Checksums cover the up SQL (or a marker for code migrations); the down
// SQL is stored verbatim instead — the copy in history is what unwinds.
inline std::int64_t checksum(const migration& m) {
  std::uint64_t h = 0xcbf29ce484222325u;  // FNV-1a 64
  auto mix = [&](std::string_view s) {
    for (unsigned char c : s) {
      h ^= c;
      h *= 0x100000001b3u;
    }
  };
  mix(m.fn ? std::string_view("<code>") : m.sql);
  return std::bit_cast<std::int64_t>(h);
}

inline std::string now_text() {
  return std::format("{:%F %T}", std::chrono::floor<std::chrono::seconds>(
                                     std::chrono::system_clock::now()));
}

// The sorted migration list (duplicates rejected) plus the sorted history.
struct changelog {
  std::vector<migration> list;
  std::vector<schema_history> applied;
};

inline result<changelog> load_changelog(db& d, std::span<const migration> in) {
  changelog c;
  c.list.assign(in.begin(), in.end());
  std::ranges::sort(c.list, {}, &migration::version);
  contract_assert(std::ranges::is_sorted(c.list, {}, &migration::version));
  for (std::size_t i = 1; i < c.list.size(); ++i)
    if (c.list[i].version == c.list[i - 1].version)
      return fail(errc::migration_duplicate,
                  std::format("two migrations have version {}", c.list[i].version));
  if (auto r = d.create_table<schema_history>(); !r)
    return std::unexpected(r.error());
  auto rows = d.query<schema_history>("ORDER BY version");
  if (!rows) return std::unexpected(rows.error());
  c.applied = std::move(*rows);
  return c;
}

inline result<void> unwind_one(db& d, const schema_history& row) {
  if (!row.down_sql)
    return fail(errc::migration_no_down,
                std::format("migration {} ('{}') recorded no down SQL",
                            row.version, row.description));
  return d.transaction([&]() -> result<void> {
    if (auto r = d.exec(*row.down_sql); !r) return r;
    return d.erase<schema_history>(row.version);
  });
}

}  // namespace detail

// Prove the changelog against history without changing anything: every
// applied version must still be in the list, with an unchanged checksum.
// Reports how many are proven and how many would run.
inline result<validate_report> validate(db& d, std::span<const migration> list)
    pre(d.open()) {
  auto c = detail::load_changelog(d, list);
  if (!c) return std::unexpected(c.error());
  validate_report report;
  for (const schema_history& row : c->applied) {
    auto it = std::ranges::find(c->list, row.version, &migration::version);
    if (it == c->list.end())
      return fail(errc::migration_missing,
                  std::format("history has version {} ('{}') but the migration "
                              "list does not", row.version, row.description));
    if (detail::checksum(*it) != row.checksum)
      return fail(errc::migration_checksum,
                  std::format("migration {} ('{}') changed after it was applied",
                              row.version, row.description));
    ++report.validated;
  }
  for (const migration& m : c->list)
    if (!std::ranges::contains(c->applied, m.version, &schema_history::version))
      ++report.pending;
  return report;
}

inline result<migrate_report> migrate(db& d, std::span<const migration> list,
                                      migrate_options opts = {})
    pre(d.open()) {
  auto c = detail::load_changelog(d, list);
  if (!c) return std::unexpected(c.error());
  migrate_report report;

  // History the binary no longer knows: unwind, newest first, with the down
  // SQL recorded at apply time — or refuse.
  std::vector<schema_history> known;
  std::vector<schema_history> unknown;
  for (schema_history& row : c->applied) {
    if (std::ranges::contains(c->list, row.version, &migration::version))
      known.push_back(std::move(row));
    else
      unknown.push_back(std::move(row));
  }
  for (auto it = unknown.rbegin(); it != unknown.rend(); ++it) {
    if (!opts.unwind_missing)
      return fail(errc::migration_missing,
                  std::format("history has version {} ('{}') but the migration "
                              "list does not (set unwind_missing to roll it back)",
                              it->version, it->description));
    if (auto r = detail::unwind_one(d, *it); !r) return std::unexpected(r.error());
    ++report.unwound;
  }

  // Prove what remains applied.
  std::int64_t newest_applied = 0;
  for (const schema_history& row : known) {
    auto it = std::ranges::find(c->list, row.version, &migration::version);
    if (detail::checksum(*it) != row.checksum)
      return fail(errc::migration_checksum,
                  std::format("migration {} ('{}') changed after it was applied",
                              row.version, row.description));
    newest_applied = std::max(newest_applied, row.version);
    ++report.validated;
  }

  // Apply what is pending.
  for (const migration& m : c->list) {
    if (std::ranges::contains(known, m.version, &schema_history::version))
      continue;
    if (m.version < newest_applied)
      return fail(errc::migration_order,
                  std::format("migration {} ('{}') is older than already-applied "
                              "version {}", m.version, m.description, newest_applied));
    auto run = d.transaction([&]() -> result<void> {
      if (m.fn) {
        if (auto r = m.fn(d); !r) return r;
      } else {
        if (auto r = d.exec(m.sql); !r) return r;
      }
      schema_history row{
          .version = m.version,
          .description = std::string(m.description),
          .checksum = detail::checksum(m),
          .applied_at = detail::now_text(),
          .down_sql = m.down.empty()
                          ? std::optional<std::string>{}
                          : std::optional<std::string>(std::string(m.down)),
      };
      return d.insert(row).transform([](std::int64_t) {});
    });
    if (!run) {
      run.error().message = std::format("migration {} ('{}'): {}", m.version,
                                        m.description, run.error().message);
      return std::unexpected(run.error());
    }
    newest_applied = m.version;
    ++report.applied;
  }
  return report;
}

// Unwind every applied migration newer than target_version, newest first,
// using the down SQL recorded when each was applied — the list is not
// needed, so an old deployment's changelog unwinds from history alone.
// rollback(d, 0) empties the changelog. Returns how many were unwound.
inline result<int> rollback(db& d, std::int64_t target_version)
    pre(d.open()) {
  if (auto r = d.create_table<schema_history>(); !r)
    return std::unexpected(r.error());
  auto rows = d.query<schema_history>("WHERE version > ? ORDER BY version DESC",
                                      target_version);
  if (!rows) return std::unexpected(rows.error());
  int n = 0;
  for (const schema_history& row : *rows) {
    if (auto r = detail::unwind_one(d, row); !r) return std::unexpected(r.error());
    ++n;
  }
  return n;
}

inline result<migrate_report> migrate(db& d, std::initializer_list<migration> list,
                                      migrate_options opts = {}) {
  return migrate(d, std::span<const migration>(list.begin(), list.end()), opts);
}
inline result<validate_report> validate(db& d, std::initializer_list<migration> list) {
  return validate(d, std::span<const migration>(list.begin(), list.end()));
}

}  // namespace salt

#endif  // SALTHERRING_HPP
