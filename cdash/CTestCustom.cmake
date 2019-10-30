set(CTEST_BUILD_NAME "undefined_BUILD_NAME")
set(CTEST_SOURCE_DIRECTORY "/Users/mdiener/Work/charm_cdash/cdash")
set(CTEST_BINARY_DIRECTORY "/Users/mdiener/Work/charm_cdash/cdash")
set(CTEST_MODEL "Experimental")
set(CTEST_SITE "MDmbp")
# set(CTEST_CHECKOUT_COMMAND "")
# set(CTEST_UPDATE_COMMAND "")
# set(CTEST_CONFIGURE_COMMAND "")
set(CTEST_BUILD_COMMAND "sh -c 'cd .. && ./build LIBS ${CTEST_BUILD_NAME} -j8 -g --with-production'")
# set(CTEST_COVERAGE_COMMAND "")
# set(CTEST_MEMORYCHECK_COMMAND "")
# set(CTEST_GENERATOR "")
# set(CTEST_GENERATOR_PLATFORM "")
set(CTEST_TIMEOUT "7200")
set(CTEST_TRIGGER "None")
set(CTEST_CUSTOM_MAXIMUM_NUMBER_OF_ERRORS "200")
set(CTEST_CUSTOM_MAXIMUM_NUMBER_OF_WARNINGS "300")
set(CTEST_CUSTOM_MAXIMUM_PASSED_TEST_OUTPUT_SIZE "104857600")
set(CTEST_CUSTOM_COVERAGE_EXCLUDE "NONE")
set(CTEST_UPDATE_TYPE "git")
set(CTEST_UPDATE_VERSION_ONLY ON)
set(CTEST_SUBMIT_RETRY_COUNT "1")
set(CTEST_SUBMIT_RETRY_DELAY "30")
set(CTEST_DROP_LOCATION "/submit.php?project=${CTEST_PROJECT_NAME}")
set(CTEST_DROP_METHOD "https")
set(CTEST_DROP_SITE "cdash.nersc.gov")
set(CTEST_NIGHTLY_START_TIME "01:00:00 UTC")
set(CTEST_DROP_SITE "my.cdash.org")

