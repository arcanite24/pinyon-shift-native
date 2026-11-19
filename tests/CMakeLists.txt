add_executable(fh1data_tests fh1data_tests.cpp)
target_link_libraries(fh1data_tests PRIVATE fh1data)
add_test(NAME fh1data_tests COMMAND fh1data_tests)
