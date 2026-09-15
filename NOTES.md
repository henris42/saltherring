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

- Checksums (FNV-1a 64) cover the up SQL; code migrations checksum a marker,
  so their bodies can be refactored freely — version them if behavior changes.
- The down SQL is not checksummed: it is *copied* into
  `salt_schema_history.down_sql` at apply time, and that stored copy is what
  `rollback()` and `unwind_missing` execute. What was applied is what
  unwinds, even from a binary that never knew the migration.
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
  `PRAGMA foreign_keys = ON` at open. Multi-statement `exec` loops on
  `sqlite3_prepare_v2`'s tail pointer.
- **Postgres**: `PQexecParams`, text parameters (binary for bytea), results
  shaped by Oid; ids come from `INSERT ... RETURNING`
  (`dialect.insert_returning`), so `last_insert_id` is never used.
- **MariaDB**: `mysql_stmt_*` prepared statements; result strings fetched
  with a zero-length probe + `mysql_stmt_fetch_column` refetch; charset 63
  distinguishes BLOB from TEXT; `my_bool` vs `bool` handled by deducing from
  `MYSQL_BIND`.
- The pg and mariadb drivers are written to their client APIs but this
  machine has neither installed — they compile only where the headers exist
  and are not yet covered by CI. The dialect SQL they run *is* unit-tested.
