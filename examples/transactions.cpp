// Transactions: rollback on error or exception, savepoints on nesting,
// write intent up front for SQLite read-modify-write.
// Build:  cmake --build build --target example_transactions

#include <saltherring/saltherring.hpp>
#include <saltherring/sqlite.hpp>

#include <cstdlib>
#include <print>

struct [[=salt::table("accounts")]] Account {
  [[=salt::pk{}]] std::int64_t id = 0;
  std::string owner;
  [[=salt::check("cents >= 0")]] std::int64_t cents = 0;
};

template <typename T>
T need(salt::result<T> r) {
  if (!r) {
    std::println(stderr, "error: {}", r.error().message);
    std::exit(1);
  }
  if constexpr (!std::is_void_v<T>) return std::move(*r);
}

int main() {
  salt::db db = need(salt::sqlite::open_memory());
  need(db.create_table<Account>());
  need(db.insert(Account{.id = 1, .owner = "alice", .cents = 10'000}));
  need(db.insert(Account{.id = 2, .owner = "bob", .cents = 500}));

  // A transfer: both updates or neither. {.immediate = true} makes SQLite
  // take the write lock before the reads, so a concurrent writer cannot
  // slip between them.
  auto transfer = [&](std::int64_t from, std::int64_t to,
                      std::int64_t amount) -> salt::result<void> {
    return db.transaction([&]() -> salt::result<void> {
      auto a = db.find<Account>(from);
      auto b = db.find<Account>(to);
      if (!a || !b) return std::unexpected((a ? b : a).error());
      if (!*a || !*b)
        return salt::fail(salt::errc::no_rows, "no such account");
      (*a)->cents -= amount;  // CHECK (cents >= 0) guards overdraft
      (*b)->cents += amount;
      if (auto r = db.update(**a); !r) return r;
      return db.update(**b);
    }, {.immediate = true});
  };

  need(transfer(1, 2, 2'500));
  std::println("after transfer: alice={} bob={}",
               need(db.find<Account>(1))->cents,
               need(db.find<Account>(2))->cents);

  // Overdraft: the CHECK fails on the first update, the guard rolls back,
  // nothing moved.
  if (auto r = transfer(2, 1, 1'000'000); !r)
    std::println("overdraft refused: {}", r.error().message);
  std::println("unchanged: alice={} bob={}",
               need(db.find<Account>(1))->cents,
               need(db.find<Account>(2))->cents);

  // Nested transactions are savepoints: the inner failure rolls back only
  // the inner work.
  need(db.transaction([&]() -> salt::result<void> {
    if (auto r = db.insert(Account{.id = 3, .owner = "carol"}); !r)
      return std::unexpected(r.error());
    auto inner = db.transaction([&]() -> salt::result<void> {
      if (auto r = db.insert(Account{.id = 4, .owner = "mallory"}); !r)
        return std::unexpected(r.error());
      return salt::fail(salt::errc::exec, "second thoughts");
    });
    std::println("inner rolled back ({}), outer continues",
                 inner.error().message);
    return {};
  }));
  std::println("carol exists: {}", need(db.find<Account>(3)).has_value());
  std::println("mallory does not: {}", !need(db.find<Account>(4)).has_value());
}
