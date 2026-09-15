// saltherring PostgreSQL driver, over libpq. Statements execute through
// PQexecParams with text-format parameters (binary for BLOB/bytea); results
// come back in text and are shaped into sql_value by column Oid. INSERT ids
// arrive via RETURNING (postgres_dialect.insert_returning), so no
// last_insert_id gymnastics.
//
// Status: written to the libpq API, compiled and exercised only where
// libpq-dev exists — not covered by the test suite on machines without it.

#ifndef SALTHERRING_PG_HPP
#define SALTHERRING_PG_HPP

#include <saltherring/saltherring.hpp>

#if __has_include(<libpq-fe.h>)
#include <libpq-fe.h>
#elif __has_include(<postgresql/libpq-fe.h>)
#include <postgresql/libpq-fe.h>
#else
#error "saltherring/pg.hpp needs libpq (install libpq-dev / postgresql-devel)"
#endif

#include <charconv>

namespace salt::pg {

namespace detail {

// Text Oids we shape into non-string sql_values.
inline constexpr unsigned oid_bool = 16, oid_bytea = 17, oid_int8 = 20,
                          oid_int2 = 21, oid_int4 = 23, oid_float4 = 700,
                          oid_float8 = 701, oid_numeric = 1700;

class statement final : public backend::statement {
 public:
  statement(PGconn* c, std::string sql) : c_(c), sql_(std::move(sql)) {}
  ~statement() override {
    if (res_) PQclear(res_);
  }
  statement(const statement&) = delete;
  statement& operator=(const statement&) = delete;

  result<void> bind(int index, const sql_value& v) override {
    if (index < 1) return fail(errc::bind, "bind index must be >= 1");
    if (params_.size() < std::size_t(index)) params_.resize(std::size_t(index));
    param& p = params_[std::size_t(index) - 1];
    p = {};
    std::visit(
        [&](const auto& x) {
          using X = std::remove_cvref_t<decltype(x)>;
          if constexpr (std::same_as<X, sql_null>) {
            p.null = true;
          } else if constexpr (std::same_as<X, std::int64_t>) {
            p.text = std::to_string(x);
          } else if constexpr (std::same_as<X, double>) {
            char buf[64];
            auto [end, ec] = std::to_chars(buf, buf + sizeof buf, x);
            p.text.assign(buf, end);
          } else if constexpr (std::same_as<X, std::string>) {
            p.text = x;
          } else {  // bytea travels binary
            p.text.assign(reinterpret_cast<const char*>(x.data()), x.size());
            p.binary = true;
          }
        },
        v);
    return {};
  }

  result<bool> step() override {
    if (!res_) {
      if (auto r = execute(); !r) return std::unexpected(r.error());
    }
    if (row_ >= PQntuples(res_)) return false;
    ++row_;
    return true;
  }

  result<sql_value> column(int index) override {
    int row = row_ - 1;
    if (row < 0 || row >= PQntuples(res_) || index >= PQnfields(res_))
      return fail(errc::exec, "column() outside a row");
    if (PQgetisnull(res_, row, index)) return sql_value{sql_null{}};
    const char* text = PQgetvalue(res_, row, index);
    std::size_t len = std::size_t(PQgetlength(res_, row, index));
    switch (PQftype(res_, index)) {
      case oid_bool:
        return sql_value{std::int64_t(text[0] == 't' || text[0] == '1')};
      case oid_int2:
      case oid_int4:
      case oid_int8: {
        std::int64_t i = 0;
        auto [p, ec] = std::from_chars(text, text + len, i);
        if (ec != std::errc{} || p != text + len)
          return fail(errc::type_mismatch, "malformed integer in result");
        return sql_value{i};
      }
      // numeric arrives as arbitrary-precision text; shaping it into double
      // is deliberate and lossy beyond 2^53 — use BIGINT/DOUBLE PRECISION
      // columns (or text) where exactness matters.
      case oid_float4:
      case oid_float8:
      case oid_numeric: {
        double d = 0;
        auto [p, ec] = std::from_chars(text, text + len, d);
        if (ec != std::errc{} || p != text + len)
          return fail(errc::type_mismatch, "malformed number in result");
        return sql_value{d};
      }
      case oid_bytea: {
        // Text-format bytea: \x-prefixed hex; anything else is refused
        // rather than guessed at.
        if (len < 2 || text[0] != '\\' || text[1] != 'x' || (len - 2) % 2 != 0)
          return fail(errc::type_mismatch, "malformed bytea in result");
        auto nib = [](char c) -> int {
          if (c >= '0' && c <= '9') return c - '0';
          char l = char(c | 0x20);
          if (l >= 'a' && l <= 'f') return l - 'a' + 10;
          return -1;
        };
        std::vector<std::uint8_t> out;
        out.reserve((len - 2) / 2);
        for (std::size_t i = 2; i + 1 < len; i += 2) {
          int hi = nib(text[i]), lo = nib(text[i + 1]);
          if (hi < 0 || lo < 0)
            return fail(errc::type_mismatch, "malformed bytea in result");
          out.push_back(std::uint8_t(hi << 4 | lo));
        }
        return sql_value{std::move(out)};
      }
      default:
        return sql_value{std::string(text, len)};
    }
  }

  int column_count() override { return res_ ? PQnfields(res_) : 0; }

 private:
  struct param {
    std::string text;
    bool null = false;
    bool binary = false;
  };

  result<void> execute() {
    std::vector<const char*> values(params_.size());
    std::vector<int> lengths(params_.size()), formats(params_.size());
    for (std::size_t i = 0; i < params_.size(); ++i) {
      values[i] = params_[i].null ? nullptr : params_[i].text.c_str();
      lengths[i] = int(params_[i].text.size());
      formats[i] = params_[i].binary ? 1 : 0;
    }
    res_ = PQexecParams(c_, sql_.c_str(), int(params_.size()), nullptr,
                        values.data(), lengths.data(), formats.data(), 0);
    auto st = PQresultStatus(res_);
    if (st != PGRES_TUPLES_OK && st != PGRES_COMMAND_OK)
      return fail(errc::exec, PQerrorMessage(c_), sql_);
    return {};
  }

  PGconn* c_;
  std::string sql_;
  std::vector<param> params_;
  PGresult* res_ = nullptr;
  int row_ = 0;
};

class connection final : public backend::connection {
 public:
  explicit connection(PGconn* c) : c_(c) {}
  ~connection() override { PQfinish(c_); }
  connection(const connection&) = delete;
  connection& operator=(const connection&) = delete;

  result<std::unique_ptr<backend::statement>> prepare(std::string_view sql) override {
    return std::unique_ptr<backend::statement>(new statement(c_, std::string(sql)));
  }

  result<void> exec(std::string_view sql) override {
    PGresult* r = PQexec(c_, std::string(sql).c_str());  // multi-statement OK
    auto st = PQresultStatus(r);
    PQclear(r);
    if (st != PGRES_TUPLES_OK && st != PGRES_COMMAND_OK)
      return fail(errc::exec, PQerrorMessage(c_), std::string(sql));
    return {};
  }

  result<std::int64_t> last_insert_id() override {
    // Unreachable through salt::db: postgres_dialect.insert_returning reads
    // the id from the INSERT ... RETURNING row instead.
    return fail(errc::exec, "postgres: use INSERT ... RETURNING");
  }

  const dialect& dial() const override { return postgres_dialect; }

 private:
  PGconn* c_;
};

}  // namespace detail

// conninfo: "host=... dbname=... user=..." or a postgresql:// URI.
inline result<db> open(const char* conninfo) {
  PGconn* c = PQconnectdb(conninfo);
  if (PQstatus(c) != CONNECTION_OK) {
    std::string msg = PQerrorMessage(c);
    PQfinish(c);
    return fail(errc::connect, std::format("postgres connect: {}", msg));
  }
  return db(std::make_unique<detail::connection>(c));
}

}  // namespace salt::pg

#endif  // SALTHERRING_PG_HPP
