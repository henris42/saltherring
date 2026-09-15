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

// [[=salt::sensitive{}]] — decode errors for this member never carry the
// stored value (error.detail is cleared and the message notes the redaction).
struct sensitive {};

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
// SQL fragments.
//
// Statement text handed to db::exec/scalar/query/query_one/count must be a
// compile-time literal: salt::sql has only a consteval constructor, so
// formatting user input into SQL does not compile. Values always travel as
// binds. The one escape hatch for genuinely runtime-assembled SQL is
// salt::unchecked_sql — deliberately ugly and greppable; a codebase that
// bans it outside one reviewed module makes injection unrepresentable.
// ---------------------------------------------------------------------------

struct sql {
  std::string_view text;
  consteval sql(const char* s) : text(s) {}
};

struct unchecked_sql {
  std::string_view text;
};

// ---------------------------------------------------------------------------
// Errors. Same shape as sardine: branch on the code, read the message.
// error.message never embeds stored values; error.detail may (capped), and
// is cleared entirely for [[=salt::sensitive{}]] members. error.sql holds
// statement text only — bound values never appear in errors.
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
  migration_tampered,  // a history row fails its own checksum — refuse to
                       // execute its stored down SQL
};

struct error {
  std::string message;  // human explanation; never contains stored values
  std::string detail;   // the offending value, when useful; capped; empty
                        // for sensitive columns
  errc code = errc::exec;
  std::string sql;      // the statement involved, when there is one; capped
};

template <typename T>
using result = std::expected<T, error>;

inline constexpr std::size_t error_text_cap = 512;

inline std::unexpected<error> fail(errc c, std::string msg, std::string sql = {}) {
  if (sql.size() > error_text_cap) sql.resize(error_text_cap);
  return std::unexpected(error{std::move(msg), {}, c, std::move(sql)});
}

// fail with a value-bearing detail (capped; kept out of message on purpose).
inline std::unexpected<error> fail_d(errc c, std::string msg,
                                     std::string_view value_detail,
                                     std::string sql = {}) {
  auto u = fail(c, std::move(msg), std::move(sql));
  u.error().detail = std::string(value_detail.substr(0, error_text_cap));
  return u;
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

// Transaction options for db::transaction. SQLite ignores iso/read_only
// (it is serializable by construction) but honors immediate — write intent
// declared up front, the read-modify-write pattern's friend. Postgres and
// MariaDB render ISOLATION LEVEL / READ ONLY into BEGIN / START TRANSACTION.
enum class isolation : std::uint8_t {
  backend_default, read_committed, repeatable_read, serializable,
};

struct tx_options {
  isolation iso = isolation::backend_default;
  bool read_only = false;
  bool immediate = false;  // SQLite: BEGIN IMMEDIATE
};

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

// std::chrono::sys_time<D> members are INTEGER columns holding the epoch
// tick count in the member's own duration (sys_seconds → epoch seconds).
template <typename T> struct is_sys_time : std::false_type {};
template <typename D>
struct is_sys_time<std::chrono::time_point<std::chrono::system_clock, D>>
    : std::true_type {};

// A 64-bit unsigned (or wider) integer cannot round-trip through SQL BIGINT:
// values above INT64_MAX would store negative. Rejected at compile time —
// store such values as text or a BLOB, deliberately.
template <typename U>
concept lossy_integral = std::integral<U> &&
    (sizeof(U) > 8 || (std::unsigned_integral<U> && sizeof(U) == 8));

// SQL identifiers from annotations: [A-Za-z_][A-Za-z0-9_]*, at most 63
// chars (the Postgres limit). quoted() does not escape, so nothing that
// would need escaping may enter — enforced at compile time.
consteval void require_identifier(std::string_view s) {
  if (s.empty() || s.size() > 63)
    throw "saltherring: identifier must be 1..63 characters";
  auto word = [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
  };
  if (s[0] >= '0' && s[0] <= '9')
    throw "saltherring: identifier may not start with a digit";
  for (char c : s)
    if (!word(c)) throw "saltherring: identifier may contain only [A-Za-z0-9_]";
}

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
  if (auto t = annotation_of<table>(^^T)) {
    require_identifier(t->str());
    return std::define_static_string(t->str());
  }
  return pascal_to_snake(std::meta::identifier_of(^^T));
}

consteval std::string_view column_name(std::meta::info m) {
  if (auto c = annotation_of<column>(m)) {
    require_identifier(c->str());
    return std::define_static_string(c->str());
  }
  return std::meta::identifier_of(m);
}

template <typename M>
consteval col_kind kind_of() {
  using U = unwrap_optional_t<M>;
  if constexpr (std::same_as<U, bool>) return col_kind::boolean;
  else if constexpr (std::is_enum_v<U>) return col_kind::enum_text;
  else if constexpr (is_sys_time<U>::value) return col_kind::integer;
  else if constexpr (std::integral<U>) {
    static_assert(!lossy_integral<U>,
        "saltherring: 64-bit unsigned (and wider) integers cannot round-trip "
        "through SQL BIGINT — store as text or a BLOB instead");
    return col_kind::integer;
  }
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
        require_identifier(r->ref_table.str());
        require_identifier(r->ref_column.str());
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
  constexpr std::string_view tn = type_name<E>();
  return fail_d(errc::unknown_enum,
                std::format("value is not an enumerator of {}", tn), s);
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
  } else if constexpr (is_sys_time<M>::value) {
    return sql_value{static_cast<std::int64_t>(v.time_since_epoch().count())};
  } else if constexpr (std::integral<M>) {
    static_assert(!lossy_integral<M>,
        "saltherring: 64-bit unsigned (and wider) integers cannot round-trip "
        "through SQL BIGINT — store as text or a BLOB instead");
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
    } else if constexpr (is_sys_time<M>::value) {
      if (auto* i = std::get_if<std::int64_t>(&v)) {
        out = M(typename M::duration(*i));
        return {};
      }
      return fail(errc::type_mismatch, "expected an INTEGER epoch time");
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
        return fail_d(errc::out_of_range, "integer does not fit the member type",
                      std::to_string(i));
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

// Rewrite user-facing '?' placeholders into the dialect's style. A real
// tokenizer, not a quote toggle: 'strings' (with '' doubling), "quoted"
// and `quoted` identifiers, -- line and (nested) /* block */ comments, and
// Postgres $tag$ dollar-quoted strings are passed through untouched. `??`
// in code position is an escape for a literal `?` (Postgres JSON operators).
// Unbalanced quotes or comments are an error, not a guess. E'…' backslash
// escapes are not modeled — bind values instead of writing them in tails.
// first is the number the first ? becomes.
inline result<std::string> adapt_placeholders(std::string_view sql_text,
                                              const dialect& d, int first = 1) {
  std::string out;
  out.reserve(sql_text.size() + 8);
  int n = first;
  enum class st : std::uint8_t {
    code, squote, dquote, bquote, line_comment, block_comment, dollar,
  };
  st state = st::code;
  int block_depth = 0;
  std::string_view dtag;  // the whole $tag$ opener
  auto word = [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
  };
  const std::size_t size = sql_text.size();
  for (std::size_t i = 0; i < size; ++i) {
    char c = sql_text[i];
    switch (state) {
      case st::code:
        if (c == '?') {
          if (i + 1 < size && sql_text[i + 1] == '?') {
            out += '?';  // ?? escape → literal ?
            ++i;
          } else {
            placeholder(out, d, n++);
          }
          continue;
        }
        if (c == '\'') state = st::squote;
        else if (c == '"') state = st::dquote;
        else if (c == '`') state = st::bquote;
        else if (c == '-' && i + 1 < size && sql_text[i + 1] == '-') {
          state = st::line_comment;
          out += "--";
          ++i;
          continue;
        } else if (c == '/' && i + 1 < size && sql_text[i + 1] == '*') {
          state = st::block_comment;
          block_depth = 1;
          out += "/*";
          ++i;
          continue;
        } else if (c == '$' && d.placeholders == placeholder_style::dollar_n) {
          std::size_t j = i + 1;
          while (j < size && word(sql_text[j])) ++j;
          if (j < size && sql_text[j] == '$') {
            dtag = sql_text.substr(i, j - i + 1);
            out += dtag;
            i = j;
            state = st::dollar;
            continue;
          }
        }
        out += c;
        continue;
      case st::squote:
        out += c;
        if (c == '\'') state = st::code;  // '' reads as exit+reenter: harmless
        continue;
      case st::dquote:
        out += c;
        if (c == '"') state = st::code;
        continue;
      case st::bquote:
        out += c;
        if (c == '`') state = st::code;
        continue;
      case st::line_comment:
        out += c;
        if (c == '\n') state = st::code;
        continue;
      case st::block_comment:
        if (c == '*' && i + 1 < size && sql_text[i + 1] == '/') {
          out += "*/";
          ++i;
          if (--block_depth == 0) state = st::code;
        } else if (c == '/' && i + 1 < size && sql_text[i + 1] == '*') {
          out += "/*";  // Postgres block comments nest
          ++i;
          ++block_depth;
        } else {
          out += c;
        }
        continue;
      case st::dollar:
        if (c == '$' && sql_text.compare(i, dtag.size(), dtag) == 0) {
          out += dtag;
          i += dtag.size() - 1;
          state = st::code;
        } else {
          out += c;
        }
        continue;
    }
  }
  if (state != st::code && state != st::line_comment)
    return fail(errc::prepare, "unbalanced quote or comment in SQL",
                std::string(sql_text));
  return out;
}

// The first statement of db::transaction, per dialect (see tx_options).
inline std::string begin_sql(const dialect& d, tx_options o) {
  if (d.name == "sqlite") return o.immediate ? "BEGIN IMMEDIATE" : "BEGIN";
  std::string_view iso =
      o.iso == isolation::read_committed  ? "READ COMMITTED"
    : o.iso == isolation::repeatable_read ? "REPEATABLE READ"
    : o.iso == isolation::serializable    ? "SERIALIZABLE"
                                          : "";
  if (d.name == "mariadb") {
    std::string out;
    if (!iso.empty())
      out = std::format("SET TRANSACTION ISOLATION LEVEL {}; ", iso);
    out += "START TRANSACTION";
    if (o.read_only) out += " READ ONLY";
    return out;
  }
  std::string out = "BEGIN";  // Postgres and standard SQL
  if (!iso.empty()) out += std::format(" ISOLATION LEVEL {}", iso);
  if (o.read_only) out += " READ ONLY";
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
  // Statement text is a compile-time literal (salt::sql); runtime-assembled
  // text must announce itself as salt::unchecked_sql. (contract_assert, not
  // pre: GCC 16.1 ICEs on pre() when a variadic member template is
  // instantiated with an empty pack.)
  template <typename... Args>
  result<void> exec(sql q, const Args&... args) {
    return exec_raw(q.text, args...);
  }
  template <typename... Args>
  result<void> exec(unchecked_sql q, const Args&... args) {
    return exec_raw(q.text, args...);
  }

  // One value from a one-row query: db.scalar<std::int64_t>("SELECT COUNT(*)...").
  template <typename V, typename... Args>
  result<V> scalar(sql q, const Args&... args) {
    return scalar_raw<V>(q.text, args...);
  }
  template <typename V, typename... Args>
  result<V> scalar(unchecked_sql q, const Args&... args) {
    return scalar_raw<V>(q.text, args...);
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
  // name", 10.0). The column list and table come from the model; the tail is
  // a literal (see salt::sql above).
  template <detail::entity T, typename... Args>
  result<std::vector<T>> query(sql tail = "", const Args&... args) {
    return query_raw<T>(tail.text, args...);
  }
  template <detail::entity T, typename... Args>
  result<std::vector<T>> query(unchecked_sql tail, const Args&... args) {
    return query_raw<T>(tail.text, args...);
  }

  // First row of a query<T>, if any.
  template <detail::entity T, typename... Args>
  result<std::optional<T>> query_one(sql tail = "", const Args&... args) {
    return first_of(query_raw<T>(tail.text, args...));
  }
  template <detail::entity T, typename... Args>
  result<std::optional<T>> query_one(unchecked_sql tail, const Args&... args) {
    return first_of(query_raw<T>(tail.text, args...));
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
  result<std::int64_t> count(sql tail = "", const Args&... args) {
    return count_raw<T>(tail.text, args...);
  }
  template <detail::entity T, typename... Args>
  result<std::int64_t> count(unchecked_sql tail, const Args&... args) {
    return count_raw<T>(tail.text, args...);
  }

  // f: () -> result<void>. BEGIN (per tx_options), run, COMMIT — with a
  // rollback guard, so an error *or an exception* out of f leaves the
  // connection outside any transaction. Nested calls become savepoints and
  // roll back only their own work.
  template <typename F>
  result<void> transaction(F&& f, tx_options o = {}) {
    contract_assert(open());
    if (!open()) return fail(errc::closed, "db is not open");
    const int depth = tx_depth_;
    std::string begin, commit, rollbk;
    if (depth == 0) {
      begin = detail::begin_sql(dial(), o);
      commit = "COMMIT";
      rollbk = "ROLLBACK";
    } else {
      std::string sp = std::format("sp_{}", depth);
      begin = "SAVEPOINT " + sp;
      commit = "RELEASE SAVEPOINT " + sp;
      rollbk = "ROLLBACK TO SAVEPOINT " + sp + "; RELEASE SAVEPOINT " + sp;
    }
    if (auto b = c_->exec(begin); !b) return b;
    ++tx_depth_;
    struct rollback_guard {
      db* self;
      const std::string* rollbk;
      bool active = true;
      ~rollback_guard() {
        if (active) {
          --self->tx_depth_;
          (void)self->c_->exec(*rollbk);
        }
      }
    } guard{this, &rollbk};
    result<void> r = std::forward<F>(f)();
    if (!r) return r;  // guard rolls back
    guard.active = false;
    --tx_depth_;
    return c_->exec(commit);
  }

  backend::connection& raw() pre(open()) { return *c_; }

 private:
  template <typename... Args>
  result<void> exec_raw(std::string_view text, const Args&... args) {
    contract_assert(open());
    if (!open()) return fail(errc::closed, "db is not open");
    auto adapted = detail::adapt_placeholders(text, dial());
    if (!adapted) return std::unexpected(adapted.error());
    if constexpr (sizeof...(Args) == 0) {
      return c_->exec(*adapted);
    } else {
      auto st = prepare_bound(*adapted, args...);
      if (!st) return std::unexpected(st.error());
      return (*st)->step().transform([](bool) {});
    }
  }

  template <typename V, typename... Args>
  result<V> scalar_raw(std::string_view text, const Args&... args) {
    contract_assert(open());
    if (!open()) return fail(errc::closed, "db is not open");
    auto adapted = detail::adapt_placeholders(text, dial());
    if (!adapted) return std::unexpected(adapted.error());
    return scalar_prepared<V>(*adapted, args...);
  }

  // The statement text is already in the dialect's placeholder style.
  template <typename V, typename... Args>
  result<V> scalar_prepared(const std::string& text, const Args&... args) {
    auto st = prepare_bound(text, args...);
    if (!st) return std::unexpected(st.error());
    auto row = (*st)->step();
    if (!row) return std::unexpected(row.error());
    if (!*row) return fail(errc::no_rows, "query produced no rows", text);
    auto v = (*st)->column(0);
    if (!v) return std::unexpected(v.error());
    V out{};
    return detail::from_sql(*v, out).transform([&] { return std::move(out); });
  }

  template <detail::entity T, typename... Args>
  result<std::vector<T>> query_raw(std::string_view tail, const Args&... args) {
    contract_assert(open());
    if (!open()) return fail(errc::closed, "db is not open");
    std::string text = detail::select_sql<T>(dial());
    if (!tail.empty()) {
      // Only the tail goes through the rewriter — the generated prefix is
      // not user text and contains no placeholders.
      auto adapted = detail::adapt_placeholders(tail, dial());
      if (!adapted) return std::unexpected(adapted.error());
      text += ' ';
      text += *adapted;
    }
    return fetch<T>(text, args...);
  }

  template <detail::entity T, typename... Args>
  result<std::int64_t> count_raw(std::string_view tail, const Args&... args) {
    contract_assert(open());
    if (!open()) return fail(errc::closed, "db is not open");
    constexpr std::string_view tname = table_of<T>();
    std::string text = "SELECT COUNT(*) FROM ";
    detail::quoted(text, tname, dial());
    if (!tail.empty()) {
      auto adapted = detail::adapt_placeholders(tail, dial());
      if (!adapted) return std::unexpected(adapted.error());
      text += ' ';
      text += *adapted;
    }
    return scalar_prepared<std::int64_t>(text, args...);
  }

  template <typename T>
  static result<std::optional<T>> first_of(result<std::vector<T>> rows) {
    if (!rows) return std::unexpected(rows.error());
    if (rows->empty()) return std::optional<T>{};
    return std::optional<T>{std::move(rows->front())};
  }

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
            if constexpr (detail::has<sensitive>(m)) {
              r.error().detail.clear();
              r.error().message += " (value redacted)";
            }
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
  int tx_depth_ = 0;
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

// The history table, itself just a persisted struct. Each row carries the
// applied migration verbatim (up and down) plus a checksum over
// version‖description‖up‖down, so both directions of drift are caught:
// a migration edited in code no longer matches its row, and a row edited
// in the database no longer matches its own checksum. FNV-1a is drift
// detection, not tamper evidence — an attacker with write access to this
// table can recompute it; cryptographic history (signing, WORM audit) is
// the application's layer, not this one.
struct [[=table("salt_schema_history")]] schema_history {
  [[=pk{}]] std::int64_t version = 0;
  std::string description;
  std::int64_t checksum = 0;
  std::int64_t checksum_rule = 0;       // which checksum formula; currently 2
  std::string applied_at;               // ISO 8601 UTC, e.g. 2026-09-15T12:00:00Z
  std::string up_sql;                   // as applied ("<code>" for fn migrations)
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

inline constexpr std::int64_t checksum_rule_current = 2;

// FNV-1a 64 over version‖description‖up‖down with field separators.
// Rule 1 (up SQL only) predates the stored-down design and is retired.
inline std::int64_t checksum_fields(std::int64_t version,
                                    std::string_view description,
                                    std::string_view up, std::string_view down) {
  std::uint64_t h = 0xcbf29ce484222325u;
  auto mix = [&](std::string_view s) {
    for (unsigned char c : s) {
      h ^= c;
      h *= 0x100000001b3u;
    }
    h ^= 0x1f;  // field separator, so "ab"+"c" != "a"+"bc"
    h *= 0x100000001b3u;
  };
  char buf[24];
  auto [end, ec] = std::to_chars(buf, buf + sizeof buf, version);
  mix(std::string_view(buf, end));
  mix(description);
  mix(up);
  mix(down);
  return std::bit_cast<std::int64_t>(h);
}

inline std::string_view up_text(const migration& m) {
  return m.fn ? std::string_view("<code>") : m.sql;
}

inline std::int64_t checksum(const migration& m) {
  return checksum_fields(m.version, m.description, up_text(m), m.down);
}

inline std::int64_t checksum(const schema_history& r) {
  return checksum_fields(r.version, r.description, r.up_sql,
                         r.down_sql ? std::string_view(*r.down_sql)
                                    : std::string_view{});
}

// A history row must match its own checksum before anything trusts it —
// in particular before its stored down SQL is executed.
inline result<void> verify_row(const schema_history& r) {
  if (r.checksum_rule != checksum_rule_current)
    return fail(errc::migration_tampered,
                std::format("history row for migration {} ('{}') uses unknown "
                            "checksum rule {}", r.version, r.description,
                            r.checksum_rule));
  if (checksum(r) != r.checksum)
    return fail(errc::migration_tampered,
                std::format("history row for migration {} ('{}') does not match "
                            "its checksum", r.version, r.description));
  return {};
}

inline std::string now_text() {
  return std::format("{:%FT%T}Z", std::chrono::floor<std::chrono::seconds>(
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
  if (auto v = verify_row(row); !v) return v;  // never run tampered down SQL
  if (!row.down_sql)
    return fail(errc::migration_no_down,
                std::format("migration {} ('{}') recorded no down SQL",
                            row.version, row.description));
  return d.transaction([&]() -> result<void> {
    if (auto r = d.exec(unchecked_sql{*row.down_sql}); !r) return r;
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
    if (auto v = detail::verify_row(row); !v) return std::unexpected(v.error());
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

  // Prove what remains applied: each row against itself, then against the
  // list.
  std::int64_t newest_applied = 0;
  for (const schema_history& row : known) {
    if (auto v = detail::verify_row(row); !v) return std::unexpected(v.error());
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
        if (auto r = d.exec(unchecked_sql{m.sql}); !r) return r;
      }
      schema_history row{
          .version = m.version,
          .description = std::string(m.description),
          .checksum = detail::checksum(m),
          .checksum_rule = detail::checksum_rule_current,
          .applied_at = detail::now_text(),
          .up_sql = std::string(detail::up_text(m)),
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
