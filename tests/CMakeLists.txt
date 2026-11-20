# carview's reference reader and metric are plain C++ with no Vulkan.
add_executable(fh1data_tests fh1data_tests.cpp ../apps/carview/reference.cpp)
target_include_directories(fh1data_tests PRIVATE ../apps/carview)
target_link_libraries(fh1data_tests PRIVATE fh1data shift_cars)
add_test(NAME fh1data_tests COMMAND fh1data_tests)

add_executable(data_tests data_tests.cpp)
target_link_libraries(data_tests PRIVATE shift_data)
add_test(NAME data_tests COMMAND data_tests)

# Counts over the player's own files (FORMAT-CARS Tests). Skipped (exit 77)
# unless SHIFT_GAME_ROOT names the disc and, for the DLC counts,
# SHIFT_STATE_ROOT the launcher state root with the installed packages.
add_test(NAME car_corpus_counts COMMAND car-corpus --check)
set_tests_properties(car_corpus_counts PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 1800 LABELS corpus)
