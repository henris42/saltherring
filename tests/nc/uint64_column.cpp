// Must NOT compile: a 64-bit unsigned member cannot round-trip through SQL
// BIGINT (values above INT64_MAX would store negative) — store such values
// as text or a BLOB, deliberately.
#include <saltherring/saltherring.hpp>

#include <cstdint>

struct [[=salt::table("nc_certs")]] Cert {
  [[=salt::pk{}]] std::int64_t id = 0;
  std::uint64_t serial = 0;  // half of all random serials would be unreadable
};

int main() {
  (void)salt::create_table_sql<Cert>(salt::sqlite_dialect);
}
