// saltherring SQLite driver. Uses <sqlite3.h> when present; otherwise
// declares the handful of entry points it needs itself (the SQLite C ABI is
// famously frozen), so only the shared library is required — link with
// -lsqlite3, or -l:libsqlite3.so.0 where the dev package is not installed.

#ifndef SALTHERRING_SQLITE_HPP
#define SALTHERRING_SQLITE_HPP

#include <saltherring/saltherring.hpp>

#if __has_include(<sqlite3.h>)
#include <sqlite3.h>
#else
extern "C" {
typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;
int sqlite3_open(const char*, sqlite3**);
int sqlite3_close(sqlite3*);
int sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
int sqlite3_bind_int64(sqlite3_stmt*, int, long long);
int sqlite3_bind_double(sqlite3_stmt*, int, double);
int sqlite3_bind_text(sqlite3_stmt*, int, const char*, int, void (*)(void*));
int sqlite3_bind_blob(sqlite3_stmt*, int, const void*, int, void (*)(void*));
int sqlite3_bind_null(sqlite3_stmt*, int);
int sqlite3_step(sqlite3_stmt*);
int sqlite3_column_type(sqlite3_stmt*, int);
long long sqlite3_column_int64(sqlite3_stmt*, int);
double sqlite3_column_double(sqlite3_stmt*, int);
const unsigned char* sqlite3_column_text(sqlite3_stmt*, int);
const void* sqlite3_column_blob(sqlite3_stmt*, int);
int sqlite3_column_bytes(sqlite3_stmt*, int);
int sqlite3_column_count(sqlite3_stmt*);
int sqlite3_finalize(sqlite3_stmt*);
const char* sqlite3_errmsg(sqlite3*);
long long sqlite3_last_insert_rowid(sqlite3*);
}
#define SQLITE_OK 0
#define SQLITE_ROW 100
#define SQLITE_DONE 101
#define SQLITE_INTEGER 1
#define SQLITE_FLOAT 2
#define SQLITE_TEXT 3
#define SQLITE_BLOB 4
#define SQLITE_NULL 5
#endif

#ifndef SQLITE_TRANSIENT
#define SQLITE_TRANSIENT reinterpret_cast<void (*)(void*)>(-1)
#endif

namespace salt::sqlite {

namespace detail {

class statement final : public backend::statement {
 public:
  statement(sqlite3* c, sqlite3_stmt* s) : c_(c), s_(s) {}
  ~statement() override { sqlite3_finalize(s_); }
  statement(const statement&) = delete;
  statement& operator=(const statement&) = delete;

  result<void> bind(int index, const sql_value& v) override {
    int rc = std::visit(
        [&](const auto& x) {
          using X = std::remove_cvref_t<decltype(x)>;
          if constexpr (std::same_as<X, sql_null>)
            return sqlite3_bind_null(s_, index);
          else if constexpr (std::same_as<X, std::int64_t>)
            return sqlite3_bind_int64(s_, index, x);
          else if constexpr (std::same_as<X, double>)
            return sqlite3_bind_double(s_, index, x);
          else if constexpr (std::same_as<X, std::string>)
            return sqlite3_bind_text(s_, index, x.data(), int(x.size()),
                                     SQLITE_TRANSIENT);
          else
            return sqlite3_bind_blob(s_, index, x.data(), int(x.size()),
                                     SQLITE_TRANSIENT);
        },
        v);
    if (rc != SQLITE_OK)
      return fail(errc::bind,
                  std::format("bind {}: {}", index, sqlite3_errmsg(c_)));
    return {};
  }

  result<bool> step() override {
    int rc = sqlite3_step(s_);
    if (rc == SQLITE_ROW) return true;
    if (rc == SQLITE_DONE) return false;
    return fail(errc::exec, sqlite3_errmsg(c_));
  }

  result<sql_value> column(int index) override {
    switch (sqlite3_column_type(s_, index)) {
      case SQLITE_NULL:
        return sql_value{sql_null{}};
      case SQLITE_INTEGER:
        return sql_value{std::int64_t(sqlite3_column_int64(s_, index))};
      case SQLITE_FLOAT:
        return sql_value{sqlite3_column_double(s_, index)};
      case SQLITE_TEXT: {
        auto* p = sqlite3_column_text(s_, index);
        int n = sqlite3_column_bytes(s_, index);
        return sql_value{std::string(reinterpret_cast<const char*>(p), std::size_t(n))};
      }
      default: {
        auto* p = static_cast<const std::uint8_t*>(sqlite3_column_blob(s_, index));
        int n = sqlite3_column_bytes(s_, index);
        return sql_value{std::vector<std::uint8_t>(p, p + n)};
      }
    }
  }

  int column_count() override { return sqlite3_column_count(s_); }

 private:
  sqlite3* c_;
  sqlite3_stmt* s_;
};

class connection final : public backend::connection {
 public:
  explicit connection(sqlite3* c) : c_(c) {}
  ~connection() override { sqlite3_close(c_); }
  connection(const connection&) = delete;
  connection& operator=(const connection&) = delete;

  result<std::unique_ptr<backend::statement>> prepare(std::string_view sql) override {
    sqlite3_stmt* s = nullptr;
    std::string z(sql);
    if (sqlite3_prepare_v2(c_, z.c_str(), -1, &s, nullptr) != SQLITE_OK)
      return fail(errc::prepare, sqlite3_errmsg(c_), std::move(z));
    return std::unique_ptr<backend::statement>(new statement(c_, s));
  }

  // Multiple ';'-separated statements: prepare/step in a loop off the tail.
  result<void> exec(std::string_view sql) override {
    std::string z(sql);
    const char* p = z.c_str();
    while (p && *p) {
      sqlite3_stmt* s = nullptr;
      const char* tail = nullptr;
      if (sqlite3_prepare_v2(c_, p, -1, &s, &tail) != SQLITE_OK)
        return fail(errc::prepare, sqlite3_errmsg(c_), std::string(p));
      if (s) {  // null for whitespace/comments
        int rc;
        while ((rc = sqlite3_step(s)) == SQLITE_ROW) {}
        sqlite3_finalize(s);
        if (rc != SQLITE_DONE)
          return fail(errc::exec, sqlite3_errmsg(c_), std::string(p));
      }
      p = tail;
    }
    return {};
  }

  result<std::int64_t> last_insert_id() override {
    return std::int64_t(sqlite3_last_insert_rowid(c_));
  }

  const dialect& dial() const override { return sqlite_dialect; }

 private:
  sqlite3* c_;
};

}  // namespace detail

// Open (creating if needed) a database file, ":memory:" for a scratch one.
// Foreign key enforcement is switched on — SQLite ships it off for history's
// sake, and [[=salt::references(...)]] deserves to mean something.
inline result<db> open(const char* path) {
  sqlite3* c = nullptr;
  if (sqlite3_open(path, &c) != SQLITE_OK) {
    std::string msg = c ? sqlite3_errmsg(c) : "out of memory";
    sqlite3_close(c);
    return fail(errc::connect, std::format("sqlite open '{}': {}", path, msg));
  }
  auto conn = std::make_unique<detail::connection>(c);
  if (auto r = conn->exec("PRAGMA foreign_keys = ON"); !r)
    return std::unexpected(r.error());
  return db(std::move(conn));
}

inline result<db> open_memory() { return open(":memory:"); }

}  // namespace salt::sqlite

#endif  // SALTHERRING_SQLITE_HPP
