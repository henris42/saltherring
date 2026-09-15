// Shared scaffolding for the server test suites (test_pg.cpp,
// test_mariadb.cpp): the EXPECT macros from test_saltherring.cpp, env-var
// connection config with defaults matching tests/containers/docker-compose.yml,
// and the skip protocol — no reachable server is a ctest SKIP (exit 77),
// never a failure, so plain `ctest` stays green on machines without Docker.

#ifndef SALTHERRING_TESTS_SERVER_FIXTURE_HPP
#define SALTHERRING_TESTS_SERVER_FIXTURE_HPP

#include <cstdlib>
#include <print>
#include <string>
#include <string_view>

static int failures = 0;

#define EXPECT(cond)                                                     \
  do {                                                                   \
    if (!(cond)) {                                                       \
      ++failures;                                                        \
      std::println("FAIL {}:{}  {}", __FILE__, __LINE__, #cond);         \
    }                                                                    \
  } while (0)

#define EXPECT_EQ(a, b)                                                  \
  do {                                                                   \
    auto va = (a);                                                       \
    auto vb = (b);                                                       \
    if (!(va == vb)) {                                                   \
      ++failures;                                                        \
      std::println("FAIL {}:{}  {} == {}\n  lhs: {}\n  rhs: {}",         \
                   __FILE__, __LINE__, #a, #b, va, vb);                  \
    }                                                                    \
  } while (0)

#define EXPECT_OK(expr)                                                  \
  do {                                                                   \
    auto&& _r = (expr);                                                  \
    if (!_r) {                                                           \
      ++failures;                                                        \
      std::println("FAIL {}:{}  {} errored: {}", __FILE__, __LINE__,     \
                   #expr, _r.error().message);                           \
    }                                                                    \
  } while (0)

namespace servertest {

inline std::string env_or(const char* name, std::string_view fallback) {
  const char* v = std::getenv(name);
  return (v && *v) ? std::string(v) : std::string(fallback);
}

// ctest is configured with SKIP_RETURN_CODE 77 for the server tests.
[[noreturn]] inline void skip(std::string_view why) {
  std::println("SKIP: {}", why);
  std::exit(77);
}

inline int finish(std::string_view suite) {
  if (failures == 0)
    std::println("all {} tests passed", suite);
  else
    std::println("{} FAILURES", failures);
  return failures != 0;
}

}  // namespace servertest

#endif  // SALTHERRING_TESTS_SERVER_FIXTURE_HPP
