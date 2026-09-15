cmake_minimum_required(VERSION 3.25)
project(saltherring LANGUAGES CXX)

# Header-only. Requires GCC 16.1+ with -freflection and -fcontracts; sardine
# must be on the include path (sibling checkout by default).
set(SALTHERRING_SARDINE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../sardine"
    CACHE PATH "Path to a sardine checkout")

add_library(saltherring INTERFACE)
add_library(saltherring::saltherring ALIAS saltherring)
target_include_directories(saltherring INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${SALTHERRING_SARDINE_DIR}/include")
target_compile_features(saltherring INTERFACE cxx_std_26)
target_compile_options(saltherring INTERFACE -freflection -fcontracts)
# The default contract-violation handler ships in libstdc++'s experimental lib.
target_link_libraries(saltherring INTERFACE stdc++exp)

if(PROJECT_IS_TOP_LEVEL)
  enable_testing()

  # The tests need only the SQLite shared library, not the dev package.
  find_library(SQLITE3_LIBRARY NAMES sqlite3)
  if(NOT SQLITE3_LIBRARY)
    set(SQLITE3_LIBRARY "-l:libsqlite3.so.0")
  endif()

  add_executable(test_saltherring tests/test_saltherring.cpp)
  target_link_libraries(test_saltherring PRIVATE saltherring ${SQLITE3_LIBRARY})
  # Designated initializers leaving defaulted members alone are idiomatic here.
  target_compile_options(test_saltherring PRIVATE -Wall -Wextra
      -Wno-missing-field-initializers)
  add_test(NAME saltherring COMMAND test_saltherring)

  # Examples double as compile checks for the documented API.
  foreach(ex quickstart columns transactions migrations)
    add_executable(example_${ex} examples/${ex}.cpp)
    target_link_libraries(example_${ex} PRIVATE saltherring ${SQLITE3_LIBRARY})
  endforeach()

  # Server suites: built where the client libraries exist. At runtime a
  # missing server is a SKIP (exit 77) so plain ctest stays green without
  # Docker; scripts/server-tests.sh runs them against the compose containers.
  find_path(LIBPQ_INCLUDE_DIR libpq-fe.h PATH_SUFFIXES postgresql)
  find_library(LIBPQ_LIBRARY pq)
  if(LIBPQ_INCLUDE_DIR AND LIBPQ_LIBRARY)
    add_executable(test_pg tests/test_pg.cpp)
    target_include_directories(test_pg PRIVATE ${LIBPQ_INCLUDE_DIR})
    target_link_libraries(test_pg PRIVATE saltherring ${LIBPQ_LIBRARY})
    target_compile_options(test_pg PRIVATE -Wall -Wextra
        -Wno-missing-field-initializers)
    add_test(NAME pg COMMAND test_pg)
    set_tests_properties(pg PROPERTIES SKIP_RETURN_CODE 77 LABELS server)
  endif()

  find_path(MARIADB_INCLUDE_DIR mysql.h PATH_SUFFIXES mariadb mysql)
  find_library(MARIADB_LIBRARY NAMES mariadb mysqlclient)
  if(MARIADB_INCLUDE_DIR AND MARIADB_LIBRARY)
    add_executable(test_mariadb tests/test_mariadb.cpp)
    target_include_directories(test_mariadb PRIVATE ${MARIADB_INCLUDE_DIR})
    target_link_libraries(test_mariadb PRIVATE saltherring ${MARIADB_LIBRARY})
    target_compile_options(test_mariadb PRIVATE -Wall -Wextra
        -Wno-missing-field-initializers)
    add_test(NAME mariadb COMMAND test_mariadb)
    set_tests_properties(mariadb PROPERTIES SKIP_RETURN_CODE 77 LABELS server)
  endif()

  # Oracle: point SALTHERRING_ORACLE_CLIENT_DIR at an Instant Client
  # directory that has sdk/include and libclntsh.so.
  set(SALTHERRING_ORACLE_CLIENT_DIR "" CACHE PATH
      "Oracle Instant Client directory (sdk/include + libclntsh.so)")
  find_path(ORACLE_INCLUDE_DIR oci.h
            HINTS "${SALTHERRING_ORACLE_CLIENT_DIR}/sdk/include")
  find_library(ORACLE_CLNTSH clntsh HINTS "${SALTHERRING_ORACLE_CLIENT_DIR}")
  if(ORACLE_INCLUDE_DIR AND ORACLE_CLNTSH)
    add_executable(test_oracle tests/test_oracle.cpp)
    target_include_directories(test_oracle PRIVATE ${ORACLE_INCLUDE_DIR})
    target_link_libraries(test_oracle PRIVATE saltherring ${ORACLE_CLNTSH})
    target_compile_options(test_oracle PRIVATE -Wall -Wextra
        -Wno-missing-field-initializers)
    get_filename_component(_ora_libdir "${ORACLE_CLNTSH}" DIRECTORY)
    # Classic DT_RPATH (not RUNPATH): libclntsh's own dependencies
    # (libclntshcore, libnnz, libaio) must resolve from the client dir too,
    # and only RPATH is transitive.
    set_target_properties(test_oracle PROPERTIES BUILD_RPATH "${_ora_libdir}")
    target_link_options(test_oracle PRIVATE -Wl,--disable-new-dtags)
    add_test(NAME oracle COMMAND test_oracle)
    set_tests_properties(oracle PROPERTIES SKIP_RETURN_CODE 77 LABELS server)
  endif()

  # Negative-compile proofs (tests/nc): each target must FAIL to build.
  # ctest builds them on demand; WILL_FAIL inverts the result.
  foreach(nc runtime_sql runtime_charptr uint64_column bad_identifier
             composite_find)
    add_library(nc_${nc} OBJECT tests/nc/${nc}.cpp)
    target_link_libraries(nc_${nc} PRIVATE saltherring)
    set_target_properties(nc_${nc} PROPERTIES EXCLUDE_FROM_ALL TRUE)
    add_test(NAME nc_${nc}
             COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}"
                     --target nc_${nc})
    set_tests_properties(nc_${nc} PROPERTIES WILL_FAIL TRUE)
  endforeach()
endif()
