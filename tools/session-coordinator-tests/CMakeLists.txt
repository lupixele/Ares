cmake_minimum_required(VERSION 3.16)
project(session_coordinator_tests LANGUAGES CXX)
enable_testing()
find_package(Threads REQUIRED)

add_executable(session_coordinator_tests
  main.cpp
  ../../src/session_display_coordinator.cpp
)

target_compile_features(session_coordinator_tests PRIVATE cxx_std_17)
target_include_directories(session_coordinator_tests PRIVATE ../..)
target_link_libraries(session_coordinator_tests PRIVATE Threads::Threads)
target_compile_options(session_coordinator_tests PRIVATE -Wall -Wextra -Werror)
foreach(case denial bounds allocation_failure publication_failure probe_failure app_failure active_stop pending_timeout pending_ready retained stale_generation restore_retry release_retry stop_failure queries exceptions destructor_handoff concurrent_retry stop_during_start view_policy view_disconnect_retain destructor_attempt_all)
  add_test(NAME coordinator_${case} COMMAND session_coordinator_tests ${case})
  set_tests_properties(coordinator_${case} PROPERTIES TIMEOUT 12)
endforeach()
