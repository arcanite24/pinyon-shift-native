add_executable(fh1data_tests fh1data_tests.cpp)
target_link_libraries(fh1data_tests PRIVATE fh1data)
add_test(NAME fh1data_tests COMMAND fh1data_tests)

add_executable(data_tests data_tests.cpp)
target_link_libraries(data_tests PRIVATE shift_data)
add_test(NAME data_tests COMMAND data_tests)
