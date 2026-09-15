// Must NOT compile: a runtime std::string is not statement text. The only
// way to pass runtime-assembled SQL is to announce it as salt::unchecked_sql.
#include <saltherring/saltherring.hpp>
#include <saltherring/sqlite.hpp>

#include <string>

struct [[=salt::table("nc_users")]] User {
  [[=salt::auto_pk]] std::int64_t id = 0;
  std::string name;
};

int main() {
  salt::db db = std::move(*salt::sqlite::open_memory());
  std::string tail = "WHERE name = 'x'";
  (void)db.query<User>(tail);  // error: no overload takes runtime text
}
