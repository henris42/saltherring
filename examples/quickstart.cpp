// Quickstart: model a struct, create its table, CRUD it.
// Build:  cmake --preset gcc16 && cmake --build build --target example_quickstart
// Run:    ./build/example_quickstart

#include <saltherring/saltherring.hpp>
#include <saltherring/sqlite.hpp>

#include <cstdlib>
#include <print>

// Annotations declare the schema; everything else is defaulted from the
// member. One [[=salt::pk{}]] (or auto_pk) enables find/update/erase by id.
struct [[=salt::table("users")]] User {
  [[=salt::auto_pk]]  std::int64_t id = 0;   // DB-assigned, written back
  [[=salt::unique{}]] std::string email;
  std::string name;
  [[=salt::check("balance >= 0")]] double balance = 0;
  std::optional<std::string> nickname;       // nullable column
};

// Every operation returns std::expected<T, salt::error>; an example may
// simply die on error where an application would branch on error().code.
template <typename T>
T need(salt::result<T> r) {
  if (!r) {
    std::println(stderr, "error: {}", r.error().message);
    std::exit(1);
  }
  if constexpr (!std::is_void_v<T>) return std::move(*r);
}

int main() {
  salt::db db = need(salt::sqlite::open_memory());  // or open("app.db")
  need(db.create_table<User>());

  // insert() binds every persisted member and writes the assigned id back.
  User henri{.email = "henri@example.com", .name = "Henri", .balance = 12.5};
  need(db.insert(henri));
  std::println("inserted henri as id {}", henri.id);

  User someone{.email = "someone@example.com", .name = "Someone",
               .balance = 40, .nickname = "so"};
  need(db.insert(someone));

  // find: absence is expected(nullopt), not an error.
  auto found = need(db.find<User>(henri.id));
  std::println("found: {} <{}>", found->name, found->email);

  // query takes a tail — the SELECT list and table come from the model.
  // The tail must be a compile-time literal; values always travel as binds.
  auto rich = need(db.query<User>("WHERE balance > ? ORDER BY balance DESC", 10.0));
  for (const User& u : rich)
    std::println("  {:>8.2f}  {}", u.balance, u.name);

  henri.balance = 99;
  henri.nickname = "hs";
  need(db.update(henri));

  std::println("users: {}", need(db.count<User>()));
  need(db.erase<User>(someone.id));
  std::println("users after erase: {}", need(db.count<User>()));

  // Constraints surface as errc::exec with the driver's message.
  User dup{.email = "henri@example.com", .name = "Impostor"};
  if (auto r = db.insert(dup); !r)
    std::println("duplicate refused: {}", r.error().message);
}
