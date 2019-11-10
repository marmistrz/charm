add_test(tests/ "make" "-C" "../tests" "test" "TESTOPTS=$ENV{AUTOBUILD_TEST_OPTS}")
set_tests_properties(tests/ PROPERTIES  TIMEOUT "1200")
add_test(examples/ "make" "-C" "../examples" "test" "TESTOPTS=$ENV{AUTOBUILD_TEST_OPTS}")
set_tests_properties(examples/ PROPERTIES  TIMEOUT "1200")
add_test(benchmarks/ "make" "-C" "../benchmarks" "test" "TESTOPTS=+$ENV{AUTOBUILD_TEST_OPTS}")
set_tests_properties(benchmarks/ PROPERTIES  TIMEOUT "1200")
