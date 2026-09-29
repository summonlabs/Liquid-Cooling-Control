# Test suite.
#
# Cases are registered by group and selected on the command line.  No test is
# given a timeout: a hanging case is a defect to be diagnosed, not masked.

add_library(lcc_test_support STATIC
  support.cpp
  child_process.cpp
)
target_link_libraries(lcc_test_support PUBLIC LiquidCooling::lcc)
target_include_directories(lcc_test_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
lcc_apply_strict_warnings(lcc_test_support)

set(LCC_TEST_SOURCES
  test_main.cpp
  test_units.cpp
  test_model.cpp
  test_determinism.cpp
  test_transitions.cpp
  test_attempts.cpp
  test_adapter.cpp
  test_store.cpp
  test_bounds.cpp
  test_concurrency.cpp
  test_multiprocess.cpp
  test_crash.cpp
  test_adversarial.cpp
  test_child_roles.cpp
)

add_executable(lcc_tests ${LCC_TEST_SOURCES})
target_link_libraries(lcc_tests PRIVATE lcc_test_support)
lcc_apply_strict_warnings(lcc_tests)

set(LCC_TEST_GROUPS
  units
  model
  determinism
  transitions
  attempts
  adapter
  store
  bounds
  concurrency
  multiprocess
  crash
  adversarial
)

foreach(group IN LISTS LCC_TEST_GROUPS)
  add_test(NAME lcc.${group} COMMAND lcc_tests ${group})
endforeach()
