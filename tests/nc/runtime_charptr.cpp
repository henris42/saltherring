// Must NOT compile: salt::sql's constructor is consteval, so even a
// const char* is rejected unless it is a constant expression.
#include <saltherring/saltherring.hpp>
#include <saltherring/sqlite.hpp>

#include <cstdlib>

int main() {
  salt::db db = std::move(*salt::sqlite::open_memory());
  const char* text = std::getenv("SQL");
  (void)db.exec(text);  // error: consteval sql(const char*) needs a constant
}
