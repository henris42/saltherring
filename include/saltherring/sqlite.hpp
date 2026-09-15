// saltherring SQLite driver. Uses <sqlite3.h> when present; otherwise
// declares the handful of entry points it needs itself (the SQLite C ABI is
// famously frozen), so only the shared library is required — link with
// -lsqlite3, or -l:libsqlite3.so.0 where the dev package is not installed.
//
// open() is hardened by default: NOFOLLOW (symlinked database paths are
// refused), URI filenames off, defensive mode on, extension loading off,
// extended result codes, a busy timeout, foreign keys ON, trusted_schema
// OFF, secure_delete ON, synchronous FULL. Each has an off switch in
// salt::sqlite::options. For CA-grade deployments prefer vendoring the
// amalgamation (SQLITE_SECURE_DELETE, SQLITE_OMIT_LOAD_EXTENSION,
// SQLITE_DQS=0) over trusting the container's shared library.

#ifndef SALTHERRING_SQLITE_HPP
#define SALTHERRING_SQLITE_HPP

#include <saltherring/saltherring.hpp>

#if __has_include(<sqlite3.h>)
#include <sqlite3.h>
#else
extern "C" {
typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;
int sqlite3_open_v2(const char*, sqlite3**, int, const char*);
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
int sqlite3_extended_result_codes(sqlite3*, int);
int sqlite3_busy_timeout(sqlite3*, int);
int sqlite3_db_config(sqlite3*, int, ...);
int sqlite3_libversion_number(void);
long long sqlite3_changes64(sqlite3*);
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

// Values from the frozen SQLite ABI, defined only if the real header did
// not provide them.
#ifndef SQLITE_TRANSIENT
#define SQLITE_TRANSIENT reinterpret_cast<void (*)(void*)>(-1)
#endif
#ifndef SQLITE_OPEN_READONLY
#define SQLITE_OPEN_READONLY 0x00000001
#define SQLITE_OPEN_READWRITE 0x00000002
#define SQLITE_OPEN_CREATE 0x00000004
#define SQLITE_OPEN_URI 0x00000040
#endif
#ifndef SQLITE_OPEN_NOFOLLOW
#define SQLITE_OPEN_NOFOLLOW 0x01000000
#endif
#ifndef SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION
#define SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION 1005
#endif
#ifndef SQLITE_DBCONFIG_DEFENSIVE
#define SQLITE_DBCONFIG_DEFENSIVE 1010
#endif
#ifndef SQLITE_CONSTRAINT
#define SQLITE_CONSTRAINT 19
#endif

namespace salt::sqlite {

// 3.37.0: sqlite3_changes64, plus everything 3.35 brought (RETURNING,
// ALTER TABLE DROP COLUMN) and every API this driver declares. Older
// libraries fail open() rather than failing mysteriously mid-migration.
inline constexpr int min_libversion = 3037000;

struct options {
  bool create = true;           // create the file if missing
  bool read_only = false;
  bool follow_symlinks = false; // NOFOLLOW unless opted out
  bool uri = false;             // allow file: URI filenames
  bool wal = false;             // PRAGMA journal_mode = WAL
  bool full_sync = true;        // PRAGMA synchronous = FULL
  bool secure_delete = true;    // PRAGMA secure_delete = ON
  int busy_timeout_ms = 5000;
};

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
            // An empty vector's data() may be null, and a null pointer here
            // binds NULL rather than a zero-length blob — keep them distinct.
            return sqlite3_bind_blob(s_, index,
                                     x.empty() ? static_cast<const void*>("")
                                               : x.data(),
                                     int(x.size()), SQLITE_TRANSIENT);
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
    // Extended result codes are on; the low byte is the primary class.
    return fail((rc & 0xff) == SQLITE_CONSTRAINT ? errc::constraint
                                                 : errc::exec,
                sqlite3_errmsg(c_));
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

  result<std::int64_t> affected() override {
    // Connection-scoped in the C API, statement-scoped by calling position:
    // salt::db reads it immediately after this statement stepped to done,
    // before anything else runs on the connection.
    return std::int64_t(sqlite3_changes64(c_));
  }

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

  // Exactly one statement. "UPDATE …; DROP TABLE …" here would silently
  // drop everything after the first ';' — refuse it instead; multi-statement
  // text belongs in exec().
  result<std::unique_ptr<backend::statement>> prepare(std::string_view sql) override {
    sqlite3_stmt* s = nullptr;
    std::string z(sql);
    const char* tail = nullptr;
    if (sqlite3_prepare_v2(c_, z.c_str(), -1, &s, &tail) != SQLITE_OK)
      return fail(errc::prepare, sqlite3_errmsg(c_), std::move(z));
    for (; tail && *tail; ++tail) {
      if (*tail != ' ' && *tail != '\t' && *tail != '\r' && *tail != '\n' &&
          *tail != ';') {
        sqlite3_finalize(s);
        return fail(errc::prepare,
                    "multiple SQL statements in prepare(); use exec()",
                    std::move(z));
      }
    }
    if (!s)  // pure whitespace/comment input
      return fail(errc::prepare, "empty statement", std::move(z));
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
          return fail((rc & 0xff) == SQLITE_CONSTRAINT ? errc::constraint
                                                       : errc::exec,
                      sqlite3_errmsg(c_), std::string(p));
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

// Open a database file (":memory:" for a scratch one), hardened per the
// header comment; every default has an off switch in options.
inline result<db> open(const char* path, options o = {}) {
  if (sqlite3_libversion_number() < min_libversion)
    return fail(errc::connect,
                std::format("sqlite library {} is older than the required {}",
                            sqlite3_libversion_number(), min_libversion));
  int flags = o.read_only
                  ? SQLITE_OPEN_READONLY
                  : SQLITE_OPEN_READWRITE | (o.create ? SQLITE_OPEN_CREATE : 0);
  if (o.uri) flags |= SQLITE_OPEN_URI;
  if (!o.follow_symlinks) flags |= SQLITE_OPEN_NOFOLLOW;
  sqlite3* c = nullptr;
  if (sqlite3_open_v2(path, &c, flags, nullptr) != SQLITE_OK) {
    std::string msg = c ? sqlite3_errmsg(c) : "out of memory";
    sqlite3_close(c);
    return fail(errc::connect, std::format("sqlite open '{}': {}", path, msg));
  }
  sqlite3_extended_result_codes(c, 1);
  sqlite3_db_config(c, SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr);
  sqlite3_db_config(c, SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION, 0, nullptr);
  sqlite3_busy_timeout(c, o.busy_timeout_ms);
  auto conn = std::make_unique<detail::connection>(c);
  std::string pragmas = "PRAGMA foreign_keys = ON; PRAGMA trusted_schema = OFF";
  if (o.secure_delete) pragmas += "; PRAGMA secure_delete = ON";
  if (!o.read_only) {
    if (o.full_sync) pragmas += "; PRAGMA synchronous = FULL";
    if (o.wal) pragmas += "; PRAGMA journal_mode = WAL";
  }
  if (auto r = conn->exec(pragmas); !r) return std::unexpected(r.error());
  return db(std::move(conn));
}

inline result<db> open_memory() { return open(":memory:"); }

}  // namespace salt::sqlite

#endif  // SALTHERRING_SQLITE_HPP
