// saltherring Oracle driver, over OCI (Oracle Call Interface, the Instant
// Client SDK's oci.h). Targets Oracle Database 23ai+ — the oracle_dialect
// emits CREATE TABLE IF NOT EXISTS (23c+) and SELECT without FROM.
//
// Oracle differences this driver surfaces rather than hides:
//   - '' IS NULL: an empty text bind is sent as NULL (engine semantics).
//     Empty BLOBs are real: blob binds travel as temporary LOBs, so a
//     zero-length blob stays distinct from NULL.
//   - No result-set RETURNING: the dialect's "INSERT ... RETURNING <id>"
//     suffix is rewritten to Oracle's RETURNING ... INTO with an out-bind,
//     and the driver synthesizes the one-value row the mapper expects.
//   - DDL commits implicitly (as on MariaDB) — the Flyway caveat applies
//     to migrations.
//   - OCI runs one statement at a time: exec() splits on top-level ';'
//     (quote- and comment-aware); text starting with BEGIN/DECLARE is sent
//     whole as a PL/SQL block.
//   - Transactions are implicit. The driver recognizes the statements
//     salt::db's transaction() emits: SET TRANSACTION opens one (statements
//     stop autocommitting), COMMIT/ROLLBACK map to OCITransCommit/Rollback,
//     SAVEPOINT / ROLLBACK TO SAVEPOINT pass through, and RELEASE SAVEPOINT
//     is a no-op (Oracle releases savepoints implicitly at commit).
//
// Status: exercised by tests/test_oracle.cpp against gvenzl/oracle-free.

#ifndef SALTHERRING_ORACLE_HPP
#define SALTHERRING_ORACLE_HPP

#include <saltherring/saltherring.hpp>

#if __has_include(<oci.h>)
#include <oci.h>
#else
#error "saltherring/oracle.hpp needs the Oracle Instant Client SDK (oci.h)"
#endif

#include <cctype>
#include <cstring>

namespace salt::oracle {

namespace detail {

inline constexpr ub2 utf8_charset = 873;  // AL32UTF8

inline std::string oci_message(OCIError* err, sb4* code_out = nullptr) {
  text buf[512];
  sb4 code = 0;
  buf[0] = 0;
  OCIErrorGet(err, 1, nullptr, &code, buf, sizeof buf, OCI_HTYPE_ERROR);
  if (code_out) *code_out = code;
  std::string m(reinterpret_cast<char*>(buf));
  while (!m.empty() && (m.back() == '\n' || m.back() == '\r')) m.pop_back();
  return m;
}

inline std::unexpected<error> oci_fail(OCIError* err, std::string sql = {}) {
  sb4 code = 0;
  std::string msg = oci_message(err, &code);
  // The integrity family: unique (1), NOT NULL (1400), FK (2291/2292),
  // CHECK (2290).
  errc c = (code == 1 || code == 1400 || code == 2290 || code == 2291 ||
            code == 2292)
               ? errc::constraint
               : errc::exec;
  return fail(c, std::move(msg), std::move(sql));
}

class connection;  // statements need the connection's tx state

class statement final : public backend::statement {
 public:
  statement(OCIEnv* env, OCIError* err, OCISvcCtx* svc, OCIStmt* s,
            const bool* in_tx, bool has_ret)
      : env_(env), err_(err), svc_(svc), s_(s), in_tx_(in_tx),
        has_ret_(has_ret) {
    ub4 n = 0;
    OCIAttrGet(s_, OCI_HTYPE_STMT, &n, nullptr, OCI_ATTR_BIND_COUNT, err_);
    params_.resize(n);  // sized ONCE: OCI keeps pointers into the elements
    ub2 t = 0;
    OCIAttrGet(s_, OCI_HTYPE_STMT, &t, nullptr, OCI_ATTR_STMT_TYPE, err_);
    is_select_ = (t == OCI_STMT_SELECT);
  }

  ~statement() override {
    for (param& p : params_) drop_lob(p.lob);
    for (col& c : cols_) drop_lob(c.lob, /*temp=*/false);
    OCIStmtRelease(s_, err_, nullptr, 0, OCI_DEFAULT);
  }
  statement(const statement&) = delete;
  statement& operator=(const statement&) = delete;

  result<void> bind(int index, const sql_value& v) override {
    if (index < 1 || std::size_t(index) > params_.size())
      return fail(errc::bind, std::format("bind index {} outside 1..{}", index,
                                          params_.size()));
    param& p = params_[std::size_t(index) - 1];
    void* valuep = nullptr;
    sb4 value_sz = 0;
    ub2 dty = SQLT_CHR;
    p.ind = 0;
    p.alen = 0;
    ub2* alenp = nullptr;

    if (std::holds_alternative<sql_null>(v)) {
      p.ind = -1;
      value_sz = 1;
      valuep = &p.i;
    } else if (auto* i = std::get_if<std::int64_t>(&v)) {
      p.i = *i;
      dty = SQLT_INT;
      valuep = &p.i;
      value_sz = sizeof p.i;
    } else if (auto* d = std::get_if<double>(&v)) {
      p.d = *d;
      dty = SQLT_BDOUBLE;
      valuep = &p.d;
      value_sz = sizeof p.d;
    } else if (auto* s = std::get_if<std::string>(&v)) {
      p.str = *s;
      if (p.str.empty()) p.ind = -1;  // '' IS NULL — Oracle semantics
      if (p.str.size() > 65535)
        return fail(errc::bind,
                    "text bind exceeds 64 KiB — Oracle VARCHAR2 caps at 4000 "
                    "(32767 extended); store large text as a BLOB");
      dty = SQLT_CHR;  // byte string with explicit length: NUL bytes survive
      valuep = p.str.data();
      value_sz = sb4(p.str.empty() ? 1 : p.str.size());
      p.alen = ub2(p.str.size());
      alenp = &p.alen;
    } else {  // blob: a temporary LOB, so empty stays distinct from NULL
      const auto& bytes = std::get<std::vector<std::uint8_t>>(v);
      drop_lob(p.lob);
      if (OCIDescriptorAlloc(env_, reinterpret_cast<void**>(&p.lob),
                             OCI_DTYPE_LOB, 0, nullptr) != OCI_SUCCESS)
        return fail(errc::bind, "OCIDescriptorAlloc failed");
      if (OCILobCreateTemporary(svc_, err_, p.lob, OCI_DEFAULT, SQLCS_IMPLICIT,
                                OCI_TEMP_BLOB, FALSE,
                                OCI_DURATION_SESSION) != OCI_SUCCESS)
        return oci_fail(err_);
      if (!bytes.empty()) {
        oraub8 byte_amt = bytes.size(), char_amt = 0;
        if (OCILobWrite2(svc_, err_, p.lob, &byte_amt, &char_amt, 1,
                         const_cast<std::uint8_t*>(bytes.data()), bytes.size(),
                         OCI_ONE_PIECE, nullptr, nullptr, 0,
                         SQLCS_IMPLICIT) != OCI_SUCCESS)
          return oci_fail(err_);
      }
      dty = SQLT_BLOB;
      valuep = &p.lob;
      value_sz = 0;
    }

    OCIBind* bnd = nullptr;
    if (OCIBindByPos(s_, &bnd, err_, ub4(index), valuep, value_sz, dty,
                     &p.ind, alenp, nullptr, 0, nullptr,
                     OCI_DEFAULT) != OCI_SUCCESS)
      return oci_fail(err_);
    return {};
  }

  result<bool> step() override {
    if (!executed_) {
      if (auto r = execute(); !r) return std::unexpected(r.error());
    }
    if (has_ret_) {  // synthesized one-value row from RETURNING ... INTO
      if (ret_consumed_) return false;
      ret_consumed_ = true;
      return true;
    }
    if (!is_select_) return false;
    sword rc = OCIStmtFetch2(s_, err_, 1, OCI_FETCH_NEXT, 0, OCI_DEFAULT);
    if (rc == OCI_NO_DATA) return false;
    if (rc != OCI_SUCCESS && rc != OCI_SUCCESS_WITH_INFO)
      return oci_fail(err_);
    return true;
  }

  result<sql_value> column(int index) override {
    if (has_ret_) {
      if (index != 0) return fail(errc::exec, "RETURNING yields one column");
      if (ret_ind_ == -1) return sql_value{sql_null{}};
      return sql_value{ret_val_};
    }
    std::size_t i = std::size_t(index);
    if (i >= cols_.size()) return fail(errc::exec, "column() outside a row");
    col& c = cols_[i];
    if (c.ind == -1) return sql_value{sql_null{}};
    switch (c.shape) {
      case col::integer: return sql_value{c.i};
      case col::real:    return sql_value{c.d};
      case col::text:    return sql_value{std::string(c.buf.data(), c.len)};
      case col::raw:
        return sql_value{std::vector<std::uint8_t>(
            c.buf.begin(), c.buf.begin() + c.len)};
      case col::blob: {
        std::vector<std::uint8_t> out;
        if (auto r = read_lob(c.lob, out); !r) return std::unexpected(r.error());
        return sql_value{std::move(out)};
      }
      case col::clob: {
        std::vector<std::uint8_t> out;
        if (auto r = read_lob(c.lob, out); !r) return std::unexpected(r.error());
        return sql_value{std::string(out.begin(), out.end())};
      }
    }
    return fail(errc::exec, "unreachable column shape");
  }

  int column_count() override {
    return has_ret_ ? 1 : int(cols_.size());
  }

  result<std::int64_t> affected() override {
    ub4 n = 0;
    OCIAttrGet(s_, OCI_HTYPE_STMT, &n, nullptr, OCI_ATTR_ROW_COUNT, err_);
    return std::int64_t(n);
  }

 private:
  struct param {
    std::int64_t i = 0;
    double d = 0;
    std::string str;
    OCILobLocator* lob = nullptr;
    sb2 ind = 0;
    ub2 alen = 0;
  };
  struct col {
    enum shape_t : std::uint8_t { integer, real, text, raw, blob, clob };
    shape_t shape = text;
    std::int64_t i = 0;
    double d = 0;
    std::vector<char> buf;
    OCILobLocator* lob = nullptr;
    ub2 len = 0;
    sb2 ind = 0;
  };

  void drop_lob(OCILobLocator*& lob, bool temp = true) {
    if (!lob) return;
    if (temp) {
      boolean is_temp = FALSE;
      if (OCILobIsTemporary(env_, err_, lob, &is_temp) == OCI_SUCCESS && is_temp)
        OCILobFreeTemporary(svc_, err_, lob);
    }
    OCIDescriptorFree(lob, OCI_DTYPE_LOB);
    lob = nullptr;
  }

  result<void> read_lob(OCILobLocator* lob, std::vector<std::uint8_t>& out) {
    oraub8 len = 0;
    if (OCILobGetLength2(svc_, err_, lob, &len) != OCI_SUCCESS)
      return std::unexpected(oci_fail(err_).error());
    out.resize(len);
    if (len == 0) return {};
    oraub8 byte_amt = len, char_amt = 0;
    if (OCILobRead2(svc_, err_, lob, &byte_amt, &char_amt, 1, out.data(),
                    out.size(), OCI_ONE_PIECE, nullptr, nullptr, 0,
                    SQLCS_IMPLICIT) != OCI_SUCCESS)
      return std::unexpected(oci_fail(err_).error());
    out.resize(byte_amt);
    return {};
  }

  // DML RETURNING ... INTO uses dynamic binds: the "in" side supplies NULL,
  // the "out" side hands OCI a buffer for the returned id.
  static sb4 ret_in_cb(void* ctx, OCIBind*, ub4, ub4, void** bufpp, ub4* alenp,
                       ub1* piecep, void** indpp) {
    auto* self = static_cast<statement*>(ctx);
    *bufpp = nullptr;
    *alenp = 0;
    *piecep = OCI_ONE_PIECE;
    self->ret_null_ind_ = -1;
    *indpp = &self->ret_null_ind_;
    return OCI_CONTINUE;
  }
  static sb4 ret_out_cb(void* ctx, OCIBind*, ub4, ub4, void** bufpp,
                        ub4** alenpp, ub1* piecep, void** indpp,
                        ub2** rcodepp) {
    auto* self = static_cast<statement*>(ctx);
    self->ret_alen_ = sizeof self->ret_val_;
    *bufpp = &self->ret_val_;
    *alenpp = &self->ret_alen_;
    *piecep = OCI_ONE_PIECE;
    *indpp = &self->ret_ind_;
    *rcodepp = &self->ret_rcode_;
    return OCI_CONTINUE;
  }

  result<void> execute() {
    if (has_ret_) {
      OCIBind* bnd = nullptr;
      if (OCIBindByName(s_, &bnd, err_,
                        reinterpret_cast<const text*>(":sr_ret"), 7, nullptr,
                        sizeof(std::int64_t), SQLT_INT, nullptr, nullptr,
                        nullptr, 0, nullptr, OCI_DATA_AT_EXEC) != OCI_SUCCESS)
        return oci_fail(err_);
      if (OCIBindDynamic(bnd, err_, this, &ret_in_cb, this,
                         &ret_out_cb) != OCI_SUCCESS)
        return oci_fail(err_);
    }
    ub4 iters = is_select_ ? 0 : 1;
    ub4 mode = (*in_tx_ || is_select_) ? OCI_DEFAULT : OCI_COMMIT_ON_SUCCESS;
    sword rc = OCIStmtExecute(svc_, s_, err_, iters, 0, nullptr, nullptr, mode);
    if (rc != OCI_SUCCESS && rc != OCI_SUCCESS_WITH_INFO)
      return oci_fail(err_);
    executed_ = true;
    if (is_select_) {
      if (auto r = describe_and_define(); !r) return r;
    }
    return {};
  }

  result<void> describe_and_define() {
    ub4 ncols = 0;
    OCIAttrGet(s_, OCI_HTYPE_STMT, &ncols, nullptr, OCI_ATTR_PARAM_COUNT, err_);
    cols_.resize(ncols);  // sized ONCE before defines take pointers
    for (ub4 i = 0; i < ncols; ++i) {
      OCIParam* parm = nullptr;
      if (OCIParamGet(s_, OCI_HTYPE_STMT, err_, reinterpret_cast<void**>(&parm),
                      i + 1) != OCI_SUCCESS)
        return oci_fail(err_);
      ub2 dtype = 0, dsize = 0;
      sb2 precision = 0;
      sb1 scale = 0;
      OCIAttrGet(parm, OCI_DTYPE_PARAM, &dtype, nullptr, OCI_ATTR_DATA_TYPE, err_);
      OCIAttrGet(parm, OCI_DTYPE_PARAM, &dsize, nullptr, OCI_ATTR_DATA_SIZE, err_);
      OCIAttrGet(parm, OCI_DTYPE_PARAM, &precision, nullptr, OCI_ATTR_PRECISION, err_);
      OCIAttrGet(parm, OCI_DTYPE_PARAM, &scale, nullptr, OCI_ATTR_SCALE, err_);
      OCIDescriptorFree(parm, OCI_DTYPE_PARAM);

      col& c = cols_[i];
      void* valuep = nullptr;
      sb4 value_sz = 0;
      ub2 dty = SQLT_CHR;
      switch (dtype) {
        case SQLT_NUM:
        case SQLT_VNU:
        case SQLT_INT:
          // NUMBER(p,0) with declared precision is integral (our columns:
          // NUMBER(19)/NUMBER(1), identity ids). Undeclared NUMBER — COUNT(*),
          // literals — fetches as BINARY_DOUBLE; from_sql's integral-REAL
          // path recovers exact integers up to 2^53.
          if (scale == 0 && precision > 0) {
            c.shape = col::integer;
            dty = SQLT_INT;
            valuep = &c.i;
            value_sz = sizeof c.i;
          } else {
            c.shape = col::real;
            dty = SQLT_BDOUBLE;
            valuep = &c.d;
            value_sz = sizeof c.d;
          }
          break;
        case SQLT_IBFLOAT:
        case SQLT_IBDOUBLE:
        case SQLT_FLT:
          c.shape = col::real;
          dty = SQLT_BDOUBLE;
          valuep = &c.d;
          value_sz = sizeof c.d;
          break;
        case SQLT_BIN:  // RAW
          c.shape = col::raw;
          c.buf.resize(std::size_t(dsize) + 1);
          dty = SQLT_BIN;
          valuep = c.buf.data();
          value_sz = sb4(c.buf.size());
          break;
        case SQLT_BLOB:
        case SQLT_CLOB:
          c.shape = (dtype == SQLT_BLOB) ? col::blob : col::clob;
          if (OCIDescriptorAlloc(env_, reinterpret_cast<void**>(&c.lob),
                                 OCI_DTYPE_LOB, 0, nullptr) != OCI_SUCCESS)
            return fail(errc::exec, "OCIDescriptorAlloc failed");
          dty = ub2(dtype);
          valuep = &c.lob;
          value_sz = 0;
          break;
        default:  // VARCHAR2/CHAR and anything else textual (dates as text)
          c.shape = col::text;
          c.buf.resize(std::size_t(dsize ? dsize : 4000) * 4 + 4);
          dty = SQLT_CHR;
          valuep = c.buf.data();
          value_sz = sb4(c.buf.size());
          break;
      }
      OCIDefine* def = nullptr;
      if (OCIDefineByPos(s_, &def, err_, i + 1, valuep, value_sz, dty, &c.ind,
                         &c.len, nullptr, OCI_DEFAULT) != OCI_SUCCESS)
        return oci_fail(err_);
    }
    return {};
  }

  OCIEnv* env_;
  OCIError* err_;
  OCISvcCtx* svc_;
  OCIStmt* s_;
  const bool* in_tx_;
  bool has_ret_ = false;
  bool is_select_ = false;
  bool executed_ = false;
  bool ret_consumed_ = false;
  std::vector<param> params_;
  std::vector<col> cols_;
  std::int64_t ret_val_ = 0;
  ub4 ret_alen_ = 0;
  sb2 ret_ind_ = 0;
  sb2 ret_null_ind_ = -1;
  ub2 ret_rcode_ = 0;
};

class connection final : public backend::connection {
 public:
  connection(OCIEnv* env, OCIError* err, OCISvcCtx* svc)
      : env_(env), err_(err), svc_(svc) {}
  ~connection() override {
    OCILogoff(svc_, err_);
    OCIHandleFree(err_, OCI_HTYPE_ERROR);
    OCIHandleFree(env_, OCI_HTYPE_ENV);
  }
  connection(const connection&) = delete;
  connection& operator=(const connection&) = delete;

  result<std::unique_ptr<backend::statement>> prepare(std::string_view sql) override {
    std::string stmt_text(sql);
    // "INSERT ... RETURNING "id"" (the dialect's insert_returning suffix)
    // becomes Oracle's RETURNING ... INTO with an out-bind.
    bool has_ret = false;
    if (auto pos = stmt_text.rfind(" RETURNING \"");
        pos != std::string::npos && stmt_text.back() == '"' &&
        stmt_text.find('"', pos + 12) == stmt_text.size() - 1) {
      stmt_text += " INTO :sr_ret";
      has_ret = true;
    }
    OCIStmt* s = nullptr;
    if (OCIStmtPrepare2(svc_, &s, err_,
                        reinterpret_cast<const text*>(stmt_text.data()),
                        ub4(stmt_text.size()), nullptr, 0, OCI_NTV_SYNTAX,
                        OCI_DEFAULT) != OCI_SUCCESS)
      return detail::oci_fail(err_, std::move(stmt_text));
    return std::unique_ptr<backend::statement>(
        new statement(env_, err_, svc_, s, &in_tx_, has_ret));
  }

  // OCI executes one statement at a time: split on top-level ';' (aware of
  // quotes and comments); BEGIN/DECLARE text travels whole as PL/SQL. The
  // transaction statements salt::db emits are recognized here.
  result<void> exec(std::string_view sql) override {
    std::string_view rest = trim(sql);
    std::string_view head = upper_prefix(rest);
    if (head.starts_with("BEGIN") || head.starts_with("DECLARE"))
      return run_one(rest);
    while (!rest.empty()) {
      std::size_t cut = top_level_semicolon(rest);
      std::string_view stmt = trim(rest.substr(0, cut));
      rest = (cut == std::string_view::npos) ? std::string_view{}
                                             : trim(rest.substr(cut + 1));
      if (stmt.empty()) continue;
      std::string_view u = upper_prefix(stmt);
      if (u.starts_with("COMMIT")) {
        if (OCITransCommit(svc_, err_, OCI_DEFAULT) != OCI_SUCCESS)
          return detail::oci_fail(err_, std::string(stmt));
        in_tx_ = false;
      } else if (u.starts_with("ROLLBACK TO")) {
        if (auto r = run_one(stmt, /*in_tx=*/true); !r) return r;
      } else if (u.starts_with("ROLLBACK")) {
        if (OCITransRollback(svc_, err_, OCI_DEFAULT) != OCI_SUCCESS)
          return detail::oci_fail(err_, std::string(stmt));
        in_tx_ = false;
      } else if (u.starts_with("RELEASE SAVEPOINT")) {
        // Oracle has no RELEASE SAVEPOINT; savepoints vanish at commit.
      } else if (u.starts_with("SET TRANSACTION")) {
        if (auto r = run_one(stmt, /*in_tx=*/true); !r) return r;
        in_tx_ = true;
      } else if (u.starts_with("SAVEPOINT")) {
        if (auto r = run_one(stmt, /*in_tx=*/true); !r) return r;
      } else {
        if (auto r = run_one(stmt); !r) return r;
      }
    }
    return {};
  }

  result<std::int64_t> last_insert_id() override {
    // Unreachable through salt::db: oracle_dialect.insert_returning routes
    // ids through RETURNING ... INTO instead.
    return fail(errc::exec, "oracle: ids arrive via INSERT ... RETURNING");
  }

  const dialect& dial() const override { return oracle_dialect; }

 private:
  static std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
      s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
      s.remove_suffix(1);
    return s;
  }

  // Uppercased first few words, enough to classify the statement.
  static std::string_view upper_prefix(std::string_view s) {
    thread_local std::string buf;
    buf.assign(s.substr(0, 24));
    for (char& c : buf) c = char(std::toupper(static_cast<unsigned char>(c)));
    return buf;
  }

  // Position of the first ';' outside quotes/comments, or npos.
  static std::size_t top_level_semicolon(std::string_view s) {
    enum { code, squote, dquote, line_c, block_c } st = code;
    for (std::size_t i = 0; i < s.size(); ++i) {
      char c = s[i];
      switch (st) {
        case code:
          if (c == ';') return i;
          if (c == '\'') st = squote;
          else if (c == '"') st = dquote;
          else if (c == '-' && i + 1 < s.size() && s[i + 1] == '-') st = line_c;
          else if (c == '/' && i + 1 < s.size() && s[i + 1] == '*') st = block_c;
          break;
        case squote: if (c == '\'') st = code; break;
        case dquote: if (c == '"') st = code; break;
        case line_c: if (c == '\n') st = code; break;
        case block_c:
          if (c == '*' && i + 1 < s.size() && s[i + 1] == '/') { st = code; ++i; }
          break;
      }
    }
    return std::string_view::npos;
  }

  result<void> run_one(std::string_view stmt, bool force_no_commit = false) {
    OCIStmt* s = nullptr;
    if (OCIStmtPrepare2(svc_, &s, err_,
                        reinterpret_cast<const text*>(stmt.data()),
                        ub4(stmt.size()), nullptr, 0, OCI_NTV_SYNTAX,
                        OCI_DEFAULT) != OCI_SUCCESS)
      return detail::oci_fail(err_, std::string(stmt));
    ub2 t = 0;
    OCIAttrGet(s, OCI_HTYPE_STMT, &t, nullptr, OCI_ATTR_STMT_TYPE, err_);
    ub4 iters = (t == OCI_STMT_SELECT) ? 0 : 1;
    ub4 mode = (in_tx_ || force_no_commit || t == OCI_STMT_SELECT)
                   ? OCI_DEFAULT
                   : OCI_COMMIT_ON_SUCCESS;
    sword rc = OCIStmtExecute(svc_, s, err_, iters, 0, nullptr, nullptr, mode);
    result<void> out{};
    if (rc != OCI_SUCCESS && rc != OCI_SUCCESS_WITH_INFO)
      out = detail::oci_fail(err_, std::string(stmt));
    OCIStmtRelease(s, err_, nullptr, 0, OCI_DEFAULT);
    return out;
  }

  OCIEnv* env_;
  OCIError* err_;
  OCISvcCtx* svc_;
  bool in_tx_ = false;

  friend result<db> open(const char*, const char*, const char*);
};

}  // namespace detail

// connect string is EZConnect: "host:port/service", e.g.
// "127.0.0.1:11521/FREEPDB1". The environment is created AL32UTF8 so unicode
// round-trips regardless of NLS_LANG.
inline result<db> open(const char* connect, const char* user,
                       const char* password) {
  OCIEnv* env = nullptr;
  if (OCIEnvNlsCreate(&env, OCI_THREADED, nullptr, nullptr, nullptr, nullptr,
                      0, nullptr, detail::utf8_charset,
                      detail::utf8_charset) != OCI_SUCCESS || !env)
    return fail(errc::connect, "OCIEnvNlsCreate failed (is the Instant Client "
                               "on the library path?)");
  OCIError* err = nullptr;
  OCIHandleAlloc(env, reinterpret_cast<void**>(&err), OCI_HTYPE_ERROR, 0, nullptr);
  OCISvcCtx* svc = nullptr;
  if (OCILogon2(env, err, &svc, reinterpret_cast<const text*>(user),
                ub4(std::strlen(user)), reinterpret_cast<const text*>(password),
                ub4(std::strlen(password)),
                reinterpret_cast<const text*>(connect),
                ub4(std::strlen(connect)), OCI_LOGON2_STMTCACHE) != OCI_SUCCESS) {
    std::string msg = detail::oci_message(err);
    OCIHandleFree(err, OCI_HTYPE_ERROR);
    OCIHandleFree(env, OCI_HTYPE_ENV);
    return fail(errc::connect, std::format("oracle connect: {}", msg));
  }
  return db(std::make_unique<detail::connection>(env, err, svc));
}

}  // namespace salt::oracle

#endif  // SALTHERRING_ORACLE_HPP
