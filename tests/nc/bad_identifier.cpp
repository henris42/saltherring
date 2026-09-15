// Must NOT compile: identifier annotations admit only [A-Za-z_][A-Za-z0-9_]*
// (max 63 chars). quoted() does not escape, so nothing escapable may enter.
#include <saltherring/saltherring.hpp>

struct [[=salt::table("us\"ers")]] Bad {
  [[=salt::pk{}]] std::int64_t id = 0;
};

int main() {
  (void)salt::create_table_sql<Bad>(salt::sqlite_dialect);
}
