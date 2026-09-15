// saltherring MariaDB/MySQL driver, over the mysql client library's prepared
// statement API (mysql_stmt_*). Parameters bind natively; results are
// fetched with growable buffers and shaped into sql_value by field type
// (binary-charset strings become BLOBs).
//
// Status: written to the MariaDB/MySQL C API, compiled and exercised only
// where the client headers exist — not covered by the test suite on
// machines without them.

#ifndef SALTHERRING_MARIADB_HPP
#define SALTHERRING_MARIADB_HPP

#include <saltherring/saltherring.hpp>

#if __has_include(<mariadb/mysql.h>)
#include <mariadb/mysql.h>
#elif __has_include(<mysql/mysql.h>)
#include <mysql/mysql.h>
#elif __has_include(<mysql.h>)
#include <mysql.h>
#else
#error "saltherring/mariadb.hpp needs the MariaDB/MySQL client headers (libmariadb-dev)"
#endif

#include <charconv>
#include <cstring>

namespace salt::mariadb {

namespace detail {

inline constexpr unsigned binary_charset = 63;  // MySQL's marker for BLOBs

// my_bool on MariaDB, bool on MySQL 8 — take whatever MYSQL_BIND uses.
using bool_t = std::remove_pointer_t<decltype(MYSQL_BIND{}.is_null)>;

class statement final : public backend::statement {
 public:
  explicit statement(MYSQL_STMT* s) : s_(s) {}
  ~statement() override { mysql_stmt_close(s_); }
  statement(const statement&) = delete;
  statement& operator=(const statement&) = delete;

  result<void> bind(int index, const sql_value& v) override {
    if (index < 1) return fail(errc::bind, "bind index must be >= 1");
    std::size_t i = std::size_t(index) - 1;
    if (params_.size() <= i) {
      params_.resize(i + 1);
      binds_.resize(i + 1);
    }
    param& p = params_[i];
    MYSQL_BIND& b = binds_[i];
    std::memset(&b, 0, sizeof b);
    std::visit(
        [&](const auto& x) {
          using X = std::remove_cvref_t<decltype(x)>;
          if constexpr (std::same_as<X, sql_null>) {
            b.buffer_type = MYSQL_TYPE_NULL;
          } else if constexpr (std::same_as<X, std::int64_t>) {
            p.i = x;
            b.buffer_type = MYSQL_TYPE_LONGLONG;
            b.buffer = &p.i;
          } else if constexpr (std::same_as<X, double>) {
            p.d = x;
            b.buffer_type = MYSQL_TYPE_DOUBLE;
            b.buffer = &p.d;
          } else {
            if constexpr (std::same_as<X, std::string>) {
              p.bytes.assign(x.begin(), x.end());
              b.buffer_type = MYSQL_TYPE_STRING;
            } else {
              p.bytes.assign(x.begin(), x.end());
              b.buffer_type = MYSQL_TYPE_BLOB;
            }
            p.len = p.bytes.size();
            b.buffer = p.bytes.data();
            b.buffer_length = p.bytes.size();
            b.length = &p.len;
          }
        },
        v);
    return {};
  }

  result<bool> step() override {
    if (!executed_) {
      if (auto r = execute(); !r) return std::unexpected(r.error());
    }
    if (!meta_) return false;  // no result set (INSERT/UPDATE/DDL)
    int rc = mysql_stmt_fetch(s_);
    if (rc == MYSQL_NO_DATA) return false;
    if (rc == 1) return fail(errc::exec, mysql_stmt_error(s_));
    return true;  // 0 or MYSQL_DATA_TRUNCATED (we refetch long columns)
  }

  result<sql_value> column(int index) override {
    std::size_t i = std::size_t(index);
    if (!meta_ || i >= cols_.size()) return fail(errc::exec, "column() outside a row");
    col& c = cols_[i];
    if (c.null) return sql_value{sql_null{}};
    switch (c.shape) {
      case col::integer:
        return sql_value{c.i};
      case col::real:
        return sql_value{c.d};
      default: {
        // Fetch the full value: the probe buffer may have truncated it.
        std::vector<std::uint8_t> data(c.len);
        if (c.len > 0) {
          MYSQL_BIND b;
          std::memset(&b, 0, sizeof b);
          b.buffer_type = MYSQL_TYPE_BLOB;
          b.buffer = data.data();
          b.buffer_length = data.size();
          if (mysql_stmt_fetch_column(s_, &b, unsigned(i), 0) != 0)
            return fail(errc::exec, mysql_stmt_error(s_));
        }
        if (c.shape == col::blob) return sql_value{std::move(data)};
        return sql_value{std::string(data.begin(), data.end())};
      }
    }
  }

  int column_count() override { return int(cols_.size()); }

 private:
  struct param {
    std::int64_t i = 0;
    double d = 0;
    std::vector<std::uint8_t> bytes;
    unsigned long len = 0;
  };
  struct col {
    enum shape_t : std::uint8_t { integer, real, text, blob } shape = text;
    std::int64_t i = 0;
    double d = 0;
    unsigned long len = 0;
    bool_t null = 0;
    bool_t err = 0;
  };

  result<void> execute() {
    if (!params_.empty() && mysql_stmt_bind_param(s_, binds_.data()) != 0)
      return fail(errc::bind, mysql_stmt_error(s_));
    if (mysql_stmt_execute(s_) != 0)
      return fail(errc::exec, mysql_stmt_error(s_));
    executed_ = true;
    meta_ = mysql_stmt_result_metadata(s_);
    if (!meta_) return {};  // statement without a result set

    unsigned n = mysql_num_fields(meta_);
    cols_.resize(n);
    out_.resize(n);
    for (unsigned i = 0; i < n; ++i) {
      MYSQL_FIELD* f = mysql_fetch_field_direct(meta_, i);
      col& c = cols_[i];
      MYSQL_BIND& b = out_[i];
      std::memset(&b, 0, sizeof b);
      switch (f->type) {
        case MYSQL_TYPE_TINY:
        case MYSQL_TYPE_SHORT:
        case MYSQL_TYPE_INT24:
        case MYSQL_TYPE_LONG:
        case MYSQL_TYPE_LONGLONG:
        case MYSQL_TYPE_YEAR:
          c.shape = col::integer;
          b.buffer_type = MYSQL_TYPE_LONGLONG;
          b.buffer = &c.i;
          break;
        case MYSQL_TYPE_FLOAT:
        case MYSQL_TYPE_DOUBLE:
        case MYSQL_TYPE_DECIMAL:
        case MYSQL_TYPE_NEWDECIMAL:
          c.shape = col::real;
          b.buffer_type = MYSQL_TYPE_DOUBLE;
          b.buffer = &c.d;
          break;
        default:
          c.shape = (f->charsetnr == binary_charset) ? col::blob : col::text;
          // Zero-length probe: fetch() reports the real length in c.len and
          // column() refetches with a right-sized buffer.
          b.buffer_type = MYSQL_TYPE_BLOB;
          b.buffer = nullptr;
          b.buffer_length = 0;
          break;
      }
      b.length = &c.len;
      b.is_null = &c.null;
      b.error = &c.err;
    }
    if (mysql_stmt_bind_result(s_, out_.data()) != 0)
      return fail(errc::exec, mysql_stmt_error(s_));
    return {};
  }

  MYSQL_STMT* s_;
  std::vector<param> params_;
  std::vector<MYSQL_BIND> binds_;
  std::vector<MYSQL_BIND> out_;
  std::vector<col> cols_;
  MYSQL_RES* meta_ = nullptr;
  bool executed_ = false;
};

class connection final : public backend::connection {
 public:
  explicit connection(MYSQL* c) : c_(c) {}
  ~connection() override { mysql_close(c_); }
  connection(const connection&) = delete;
  connection& operator=(const connection&) = delete;

  result<std::unique_ptr<backend::statement>> prepare(std::string_view sql) override {
    MYSQL_STMT* s = mysql_stmt_init(c_);
    if (!s) return fail(errc::prepare, "mysql_stmt_init: out of memory");
    if (mysql_stmt_prepare(s, sql.data(), sql.size()) != 0) {
      std::string msg = mysql_stmt_error(s);
      mysql_stmt_close(s);
      return fail(errc::prepare, std::move(msg), std::string(sql));
    }
    return std::unique_ptr<backend::statement>(new statement(s));
  }

  result<void> exec(std::string_view sql) override {
    // CLIENT_MULTI_STATEMENTS is on: drain every result set.
    if (mysql_real_query(c_, sql.data(), sql.size()) != 0)
      return fail(errc::exec, mysql_error(c_), std::string(sql));
    do {
      if (MYSQL_RES* r = mysql_store_result(c_)) mysql_free_result(r);
    } while (mysql_next_result(c_) == 0);
    return {};
  }

  result<std::int64_t> last_insert_id() override {
    return std::int64_t(mysql_insert_id(c_));
  }

  const dialect& dial() const override { return mariadb_dialect; }

 private:
  MYSQL* c_;
};

}  // namespace detail

inline result<db> open(const char* host, const char* user, const char* password,
                       const char* database, unsigned port = 3306) {
  MYSQL* c = mysql_init(nullptr);
  if (!c) return fail(errc::connect, "mysql_init: out of memory");
  if (!mysql_real_connect(c, host, user, password, database, port, nullptr,
                          CLIENT_MULTI_STATEMENTS)) {
    std::string msg = mysql_error(c);
    mysql_close(c);
    return fail(errc::connect, std::format("mariadb connect: {}", msg));
  }
  mysql_set_character_set(c, "utf8mb4");
  return db(std::make_unique<detail::connection>(c));
}

}  // namespace salt::mariadb

#endif  // SALTHERRING_MARIADB_HPP
