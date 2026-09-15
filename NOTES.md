# Notes

## Java-library parity

| Java | saltherring |
|---|---|
| JPA `@Entity` / `@Table(name)` | any struct / `[[=salt::table("x")]]` |
| `@Id` | `[[=salt::pk{}]]` |
| `@GeneratedValue(IDENTITY)` | `[[=salt::auto_pk]]` — id written back on insert |
| `@Column(name)` | `[[=salt::column("x")]]` |
| `@Transient` | `[[=salt::transient{}]]` |
| `@Column(unique=true)` | `[[=salt::unique{}]]` |
| `@Index` | `[[=salt::indexed{}]]` |
| `@Check` (Hibernate) | `[[=salt::check("expr")]]` |
| `@ColumnDefault` | `[[=salt::sql_default("expr")]]` |
| `@ManyToOne` + FK | `[[=salt::references("table", "col")]]` — the id, not a proxy |
| nullable = `Optional`/wrapper | `std::optional<T>` ↔ nullable column |
| `@Convert` / JSON columns | automatic: any sardine type → sardine JSON TEXT |
| `EntityManager` persist/find/merge/remove | `db.insert / find / update / erase` |
| JPQL/Criteria | `db.query<T>("WHERE ... ?", args...)` — SQL tail, typed binds |
| Flyway `migrate` / `validate` | `salt::migrate` / `salt::validate` |
| Flyway `V1__desc.sql` + checksum | `salt::migration{version, description, sql}` + FNV-1a |
| Liquibase `rollback` | `salt::rollback(db, target)` — down SQL stored in history |
| Flyway `flyway_schema_history` | `salt_schema_history` (a reflected struct itself) |

Deliberately absent (v1): relations/lazy loading (store ids, join in SQL),
a query DSL, connection pooling, an L2 cache. The Java features those mimic
are the ones that page people at 3am.

## sardine compatibility contract

- Enum columns store `sardine::detail::json_name` of the enumerator —
  `sardine::rename` / `rename_all` on the enum change the stored text;
  `sardine::enum_from_number` additionally lets INTEGER (or numeric text)
  columns decode.
- A sequence of exactly `std::uint8_t` is a BLOB — same dispatch rule as
  sardine's CBOR byte strings.
- Any other non-scalar member is `sardine::to_json`/`from_json` in a TEXT
  column; sardine's field annotations apply inside that document.
- `salt::column` naming is independent of `sardine::rename` on purpose: the
  wire name and the column name are different contracts. The member
  identifier is the shared default.
- saltherring reaches into `sardine::detail` (annotation lookup, member
  iteration, json_name). Same-author libraries, versioned together.

## Changelog handling

- Checksums (FNV-1a 64, rule 2) cover version‖description‖up‖down with field
  separators; code migrations checksum a `<code>` marker, so their bodies can
  be refactored freely — version them if behavior changes. Rule 1 (up SQL
  only) predates the stored-down design and is retired; rows carrying an
  unknown `checksum_rule` are refused as tampered.
- The down SQL is *copied* into `salt_schema_history.down_sql` at apply time,
  and that stored copy is what `rollback()` and `unwind_missing` execute —
  what was applied is what unwinds, even from a binary that never knew the
  migration. Before any stored down SQL runs, the row is re-verified against
  its own checksum (`errc::migration_tampered` on mismatch), so write access
  to the history table is not silently SQL execution at the next rollback.
  FNV-1a is drift detection, not tamper evidence — see README's security
  model.
- `migrate()` per-migration transaction: SQLite and Postgres give
  transactional DDL; MariaDB DDL self-commits (same caveat as Flyway).
- Out-of-order pending migrations are refused (Flyway's default), duplicates
  are refused before anything runs.

## GCC 16.1 notes

Everything from sardine's NOTES.md applies (annotations must be structural,
`[[=name("x")]]` call syntax, `define_static_array` around member vectors),
plus what this library hit:

- **Contracts on template members ICE.** `pre()` on a variadic member
  template segfaults at substitution with an empty pack; `pre()` on other
  member templates ICEs in gimplification (`gimple_add_tmp_var`). Contracts
  here: `pre`/`post` only on non-template functions, `contract_assert` as
  the first statement of template bodies. Semantics match under the default
  enforce semantic.
- **Link `stdc++exp`.** The default contract-violation handler lives in
  libstdc++'s experimental archive; without it every TU using contracts
  fails to link.
- **Hoist consteval calls out of dependent runtime expressions.** GCC
  rejects `std::string(json_name<E>(e))` in a template-for body at runtime
  ("consteval-only expression"), even though the call is an immediate
  invocation of constants. `constexpr auto n = json_name<E>(e);` first, then
  use `n`.
- Postconditions may not name non-const value parameters; take them const or
  assert on locals.

## Driver notes

- **SQLite**: works without `sqlite3.h` — the header declares the dozen
  entry points itself (frozen ABI) and links `-l:libsqlite3.so.0`.
  Hardened open by default (see README); `prepare()` refuses trailing
  statements via the tail pointer, multi-statement `exec` loops on it.
  An empty blob must not bind through a null `data()` pointer —
  `sqlite3_bind_blob(…, nullptr, 0, …)` binds NULL, not a zero-length
  blob (caught by the conformance suite).
- **Postgres**: `PQexecParams`, text parameters (binary for bytea), results
  shaped by Oid; ids come from `INSERT ... RETURNING`
  (`dialect.insert_returning`), so `last_insert_id` is never used. A text
  parameter with an embedded NUL is refused at bind (`errc::bind`) —
  Postgres TEXT cannot store it, and text-format params would silently
  truncate at the NUL otherwise.
- **MariaDB**: `mysql_stmt_*` prepared statements; result strings fetched
  with a zero-length probe + `mysql_stmt_fetch_column` refetch; charset 63
  distinguishes BLOB from TEXT; `my_bool` vs `bool` handled by deducing from
  `MYSQL_BIND`. Connects with `CLIENT_FOUND_ROWS` so `execute()` reports
  matched rows like the other drivers, and utf8mb4 for full unicode.
- Bugs the first container runs caught (2026-09-15), for pattern-matching
  in future drivers: MariaDB param arrays must be sized once up front —
  `MYSQL_BIND` keeps raw pointers into them and a `resize()` between
  `bind()` calls dangles every earlier buffer (garbage values that often
  *happen* to read back correctly); a second statement's error in
  multi-statement `exec` arrives via `mysql_next_result() > 0`, not from
  `mysql_real_query`; empty string/blob binds must not pass a null buffer
  (sends NULL — same bug class as SQLite's empty-blob bind); MariaDB
  `TEXT`/`BLOB` cap at 64 KiB, so the dialect uses LONGTEXT/LONGBLOB.
- **Oracle** (23ai+, over OCI / Instant Client): identifiers quoted
  UPPERCASE (`dialect.fold_upper`) so unquoted tails — which Oracle folds
  to upper — keep matching; `''` IS NULL surfaced, not hidden (empty text
  binds send NULL; empty *blobs* stay real via temporary-LOB binds);
  `INSERT ... RETURNING "ID"` rewritten to `RETURNING ... INTO :sr_ret`
  with an OCIBindDynamic out-bind (last_insert_id has no table context, so
  currval was not an option); exec() splits top-level ';' client-side and
  sends BEGIN/DECLARE text whole as PL/SQL; RELEASE SAVEPOINT is a no-op
  (Oracle has none); transactions are recognized from the statements
  salt::db emits (SET TRANSACTION / COMMIT / ROLLBACK) and everything else
  autocommits via OCI_COMMIT_ON_SUCCESS; NUMBER with declared precision
  and scale 0 fetches as int64, undeclared NUMBER (COUNT(*), literals) as
  BINARY_DOUBLE — from_sql's integral-REAL path recovers counts exactly.
  Env is created AL32UTF8 explicitly so unicode survives without NLS_LANG.
  Build against an Instant Client dir (`SALTHERRING_ORACLE_CLIENT_DIR`);
  the test binary links with classic DT_RPATH (`--disable-new-dtags`)
  because libclntsh's own deps resolve transitively only through RPATH.
  Ubuntu 24.04's libaio1t64 renamed the soname to `libaio.so.1t64`, so the
  client dir carries a `libaio.so.1` symlink to it (the t64 transition's
  standard workaround; a benign "no version information" warning remains).
- All server drivers pass the conformance suite and their dialect suites
  against real containers: `scripts/server-tests.sh`, or
  `tests/containers/docker-compose.yml` + `ctest -L server` by hand
  (postgres:17-alpine, mariadb:11.4, gvenzl/oracle-free:23-slim).
  No reachable server = SKIP (exit 77), so plain ctest needs no Docker.
  CI with a GCC 16.1 toolchain image is planned
  (internal/server-test-plan.md, phase 3).
