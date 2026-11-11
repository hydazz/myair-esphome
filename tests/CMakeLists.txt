cmake_minimum_required(VERSION 3.20)
project(myair_tests LANGUAGES CXX)
enable_testing()
add_executable(myair_tests
  test_myair.cpp
  test_controls.cpp
  test_activity.cpp
  ../components/myair/protocol.cpp
  ../components/myair/controller.cpp
  ../components/myair/activity.cpp
  ../components/myair/session.cpp
)
target_compile_features(myair_tests PRIVATE cxx_std_17)
target_compile_options(myair_tests PRIVATE -Wall -Wextra -Werror)
target_include_directories(myair_tests PRIVATE ../components/myair)
add_test(NAME myair COMMAND myair_tests)
set_tests_properties(myair PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
