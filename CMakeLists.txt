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

  # Negative-compile proofs (tests/nc): each target must FAIL to build.
  # ctest builds them on demand; WILL_FAIL inverts the result.
  foreach(nc runtime_sql runtime_charptr uint64_column bad_identifier)
    add_library(nc_${nc} OBJECT tests/nc/${nc}.cpp)
    target_link_libraries(nc_${nc} PRIVATE saltherring)
    set_target_properties(nc_${nc} PROPERTIES EXCLUDE_FROM_ALL TRUE)
    add_test(NAME nc_${nc}
             COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}"
                     --target nc_${nc})
    set_tests_properties(nc_${nc} PROPERTIES WILL_FAIL TRUE)
  endforeach()
endif()
