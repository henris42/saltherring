// Column encoding is shared with sardine: enums store their wire name,
// uint8_t sequences store as BLOBs, sys_time members store epoch ticks in
// an INTEGER, and any other sardine-serializable member rides in a TEXT
// column as sardine JSON.
// Build:  cmake --build build --target example_columns

#include <saltherring/saltherring.hpp>
#include <saltherring/sqlite.hpp>

#include <chrono>
#include <cstdlib>
#include <print>

// sardine annotations govern the stored text of enumerators.
enum class [[=sardine::rename_all("SCREAMING_SNAKE_CASE")]] Level {
  debug_info,
  warning,
  fatal_error,
};

struct Address {  // no salt annotations needed — becomes a JSON TEXT column
  std::string street;
  std::string city;
};

struct [[=salt::table("events")]] Event {
  [[=salt::auto_pk]] std::int64_t id = 0;
  [[=salt::indexed{}]] Level level = Level::warning;  // TEXT: "WARNING"
  Address addr;                                       // TEXT: sardine JSON
  std::vector<int> readings;                          // TEXT: sardine JSON
  std::vector<std::uint8_t> payload;                  // BLOB
  std::chrono::sys_seconds at{};                      // INTEGER: epoch seconds
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
  need(db.create_table<Event>());

  Event e{.level = Level::fatal_error,
          .addr = {.street = "Mannerheimintie 1", .city = "Helsinki"},
          .readings = {1, 2, 3},
          .payload = {0xde, 0xad, 0xbe, 0xef},
          .at = std::chrono::sys_seconds(std::chrono::seconds(1757900000))};
  need(db.insert(e));

  // What actually landed in the columns:
  std::println("level column: {}",
               need(db.scalar<std::string>("SELECT level FROM events")));
  std::println("addr column:  {}",
               need(db.scalar<std::string>("SELECT addr FROM events")));
  std::println("at column:    {}",
               need(db.scalar<std::int64_t>("SELECT at FROM events")));

  // And it all round-trips through the mapper.
  Event back = *need(db.find<Event>(e.id));
  std::println("round-trip: level ok={} street='{}' readings={} payload bytes={}",
               back.level == Level::fatal_error, back.addr.street,
               back.readings.size(), back.payload.size());

  // The generated DDL is a pure function of (model, dialect) — inspect it
  // for any backend without a connection.
  std::println("\npostgres DDL:\n{}",
               salt::create_table_sql<Event>(salt::postgres_dialect));
}
