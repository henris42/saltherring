// saltherring driver conformance suite — the backend::connection contract as
// executable checks, parameterised on a db factory. Every driver is expected
// to pass: the shared storage classes round-trip at their edges, NULL stays
// distinct from empty, prepare() takes exactly one statement while exec()
// takes many, transactions roll back on error, and nested transactions are
// savepoints. The SQLite driver runs it from test_saltherring.cpp; a server
// driver (pg.hpp, mariadb.hpp, or an application's own) links this same
// header against its own factory in its own harness.
//
// NaN is deliberately absent: backends disagree (SQLite stores NaN as NULL),
// so its policy is per-driver, not part of the contract.

#ifndef SALTHERRING_TESTS_CONFORMANCE_HPP
#define SALTHERRING_TESTS_CONFORMANCE_HPP

#include <saltherring/saltherring.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <print>
#include <string>
#include <vector>

namespace salt::conformance {

struct [[=salt::table("conf_kinds")]] kinds {
  [[=salt::auto_pk]] std::int64_t id = 0;
  bool flag = false;
  std::int8_t tiny = 0;
  std::uint32_t wide = 0;
  std::int64_t big = 0;
  double real = 0;
  std::string text;
  std::vector<std::uint8_t> bytes;
  std::optional<std::string> maybe;
  std::chrono::sys_seconds at{};
};

// Runs the whole suite against one fresh connection from the factory.
// Returns the number of failed checks (0 = conformant), one line per failure.
inline int run(const std::function<salt::db()>& fresh_db) {
  int failures = 0;
  auto check = [&](bool ok, std::string_view what) {
    if (!ok) {
      ++failures;
      std::println("CONFORMANCE FAIL: {}", what);
    }
  };

  salt::db db = fresh_db();
  check(db.open(), "factory returns an open db");
  if (!db.open()) return failures;
  check(bool(db.create_table<kinds>()), "create_table");

  // Storage classes at their edges; text may hold unicode and NUL bytes.
  std::string tricky = "häräntappoase 🐟 ";
  tricky.push_back('\0');
  tricky += "end";
  kinds k{.flag = true,
          .tiny = std::numeric_limits<std::int8_t>::min(),
          .wide = std::numeric_limits<std::uint32_t>::max(),
          .big = std::numeric_limits<std::int64_t>::min(),
          .real = 2.5,
          .text = tricky,
          .bytes = {},  // empty BLOB, not NULL
          .maybe = std::nullopt,
          .at = std::chrono::sys_seconds(std::chrono::seconds(1757900000))};
  auto id = db.insert(k);
  check(bool(id), "insert returns the assigned id");
  check(k.id != 0, "auto pk written back into the object");

  auto got = db.find<kinds>(k.id);
  check(got && got->has_value(), "find by pk");
  if (got && *got) {
    const kinds& g = **got;
    check(g.flag == k.flag, "bool round-trip");
    check(g.tiny == k.tiny, "int8 min round-trip");
    check(g.wide == k.wide, "uint32 max round-trip");
    check(g.big == k.big, "int64 min round-trip");
    check(g.real == k.real, "double round-trip");
    check(g.text == k.text, "unicode + NUL text round-trip");
    check(g.bytes.empty(), "empty blob round-trip");
    check(!g.maybe.has_value(), "NULL optional round-trip");
    check(g.at == k.at, "sys_seconds round-trip");
  }

  // Empty string stays distinct from NULL; int64 max survives.
  kinds k2{.big = std::numeric_limits<std::int64_t>::max(),
           .maybe = std::string{}};
  check(bool(db.insert(k2)), "second insert");
  auto got2 = db.find<kinds>(k2.id);
  check(got2 && got2->has_value(), "find second row");
  if (got2 && *got2) {
    check((*got2)->big == std::numeric_limits<std::int64_t>::max(),
          "int64 max round-trip");
    check((*got2)->maybe.has_value() && (*got2)->maybe->empty(),
          "empty string is not NULL");
  }

  // A 1 MiB blob.
  kinds kb{.bytes = std::vector<std::uint8_t>(1 << 20)};
  for (std::size_t i = 0; i < kb.bytes.size(); ++i)
    kb.bytes[i] = std::uint8_t(i * 131);
  check(bool(db.insert(kb)), "1 MiB blob insert");
  auto gb = db.find<kinds>(kb.id);
  check(gb && *gb && (*gb)->bytes == kb.bytes, "1 MiB blob round-trip");

  // Bulk: 10 000 rows in one transaction, all fetched back.
  auto bulk = db.transaction([&]() -> salt::result<void> {
    for (int i = 0; i < 10'000; ++i) {
      kinds r{.big = i};
      if (auto ins = db.insert(r); !ins) return std::unexpected(ins.error());
    }
    return {};
  });
  check(bool(bulk), "bulk insert transaction");
  check(db.count<kinds>().value_or(-1) == 10'003, "10k rows counted");
  auto rows = db.query<kinds>("ORDER BY id");
  check(rows && rows->size() == 10'003, "10k rows fetched");

  // prepare() is one statement; exec() is many.
  check(!db.scalar<std::int64_t>("SELECT 1; SELECT 2"),
        "prepare refuses a second statement");
  check(bool(db.exec("CREATE TABLE conf_two (x INTEGER); "
                     "INSERT INTO conf_two VALUES (1)")),
        "exec runs multiple statements");
  check(db.scalar<std::int64_t>("SELECT COUNT(*) FROM conf_two").value_or(-1) == 1,
        "multi-statement exec really ran");

  // Transactions roll back on error; nested ones are savepoints.
  auto before = db.count<kinds>().value_or(-1);
  auto tx = db.transaction([&]() -> salt::result<void> {
    kinds r{};
    if (auto ins = db.insert(r); !ins) return std::unexpected(ins.error());
    return salt::fail(salt::errc::exec, "abort on purpose");
  });
  check(!tx, "failed transaction reports its error");
  check(db.count<kinds>().value_or(-1) == before, "failed transaction rolled back");

  auto nested = db.transaction([&]() -> salt::result<void> {
    kinds outer{};
    if (auto ins = db.insert(outer); !ins) return std::unexpected(ins.error());
    auto inner = db.transaction([&]() -> salt::result<void> {
      kinds in{};
      if (auto ins = db.insert(in); !ins) return std::unexpected(ins.error());
      return salt::fail(salt::errc::exec, "inner abort");
    });
    if (inner) return salt::fail(salt::errc::exec, "inner should have failed");
    return {};
  });
  check(bool(nested), "outer transaction commits around a failed savepoint");
  check(db.count<kinds>().value_or(-1) == before + 1,
        "savepoint rolled back only the inner work");

  return failures;
}

}  // namespace salt::conformance

#endif  // SALTHERRING_TESTS_CONFORMANCE_HPP
