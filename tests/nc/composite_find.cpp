// Must NOT compile: find/update/erase key on THE primary key, and a
// composite-keyed entity has no single one — those tables are read with
// query()/query_one() and written with exec()/execute(), where the WHERE
// clause names both columns explicitly.
#include <saltherring/saltherring.hpp>
#include <saltherring/sqlite.hpp>

#include <string>

struct [[=salt::table("nc_pairs")]] Pair {
  [[=salt::pk{}]] std::string tenant_id;
  [[=salt::pk{}]] std::string id;
  std::string body;
};

int main() {
  auto db = salt::sqlite::open_memory();
  (void)db->find<Pair>(std::string("half-a-key"));
}
