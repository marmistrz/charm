add_test(tests/ "make" "-C" "../tests" "test" "TESTOPTS=++local")
set_tests_properties(tests/ PROPERTIES  TIMEOUT "1200")
add_test(examples/ "make" "-C" "../examples" "test" "TESTOPTS=++local")
set_tests_properties(examples/ PROPERTIES  TIMEOUT "1200")
add_test(benchmarks/ "make" "-C" "../benchmarks" "test" "TESTOPTS=++local")
set_tests_properties(benchmarks/ PROPERTIES  TIMEOUT "1200")
