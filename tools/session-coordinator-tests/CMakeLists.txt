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
add_test(NAME session_coordinator_tests COMMAND session_coordinator_tests)
