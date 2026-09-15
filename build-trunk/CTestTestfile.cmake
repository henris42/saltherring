# CMake generated Testfile for 
# Source directory: /home/hs/saltherring
# Build directory: /home/hs/saltherring/build-trunk
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test([=[saltherring]=] "/home/hs/saltherring/build-trunk/test_saltherring")
set_tests_properties([=[saltherring]=] PROPERTIES  _BACKTRACE_TRIPLES "/home/hs/saltherring/CMakeLists.txt;33;add_test;/home/hs/saltherring/CMakeLists.txt;0;")
add_test([=[nc_runtime_sql]=] "/usr/bin/cmake" "--build" "/home/hs/saltherring/build-trunk" "--target" "nc_runtime_sql")
set_tests_properties([=[nc_runtime_sql]=] PROPERTIES  WILL_FAIL "TRUE" _BACKTRACE_TRIPLES "/home/hs/saltherring/CMakeLists.txt;48;add_test;/home/hs/saltherring/CMakeLists.txt;0;")
add_test([=[nc_runtime_charptr]=] "/usr/bin/cmake" "--build" "/home/hs/saltherring/build-trunk" "--target" "nc_runtime_charptr")
set_tests_properties([=[nc_runtime_charptr]=] PROPERTIES  WILL_FAIL "TRUE" _BACKTRACE_TRIPLES "/home/hs/saltherring/CMakeLists.txt;48;add_test;/home/hs/saltherring/CMakeLists.txt;0;")
add_test([=[nc_uint64_column]=] "/usr/bin/cmake" "--build" "/home/hs/saltherring/build-trunk" "--target" "nc_uint64_column")
set_tests_properties([=[nc_uint64_column]=] PROPERTIES  WILL_FAIL "TRUE" _BACKTRACE_TRIPLES "/home/hs/saltherring/CMakeLists.txt;48;add_test;/home/hs/saltherring/CMakeLists.txt;0;")
add_test([=[nc_bad_identifier]=] "/usr/bin/cmake" "--build" "/home/hs/saltherring/build-trunk" "--target" "nc_bad_identifier")
set_tests_properties([=[nc_bad_identifier]=] PROPERTIES  WILL_FAIL "TRUE" _BACKTRACE_TRIPLES "/home/hs/saltherring/CMakeLists.txt;48;add_test;/home/hs/saltherring/CMakeLists.txt;0;")
add_test([=[nc_composite_find]=] "/usr/bin/cmake" "--build" "/home/hs/saltherring/build-trunk" "--target" "nc_composite_find")
set_tests_properties([=[nc_composite_find]=] PROPERTIES  WILL_FAIL "TRUE" _BACKTRACE_TRIPLES "/home/hs/saltherring/CMakeLists.txt;48;add_test;/home/hs/saltherring/CMakeLists.txt;0;")
