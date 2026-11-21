# carview's reference reader and metric are plain C++ with no Vulkan.
add_executable(fh1data_tests fh1data_tests.cpp ../apps/carview/reference.cpp)
target_include_directories(fh1data_tests PRIVATE ../apps/carview)
target_link_libraries(fh1data_tests PRIVATE fh1data shift_cars)
add_test(NAME fh1data_tests COMMAND fh1data_tests)

add_executable(drive_tests drive_tests.cpp)
target_link_libraries(drive_tests PRIVATE shift_drive)
add_test(NAME drive_tests COMMAND drive_tests)

# SPEC-ENGINE-SOUND: the rule checks, the reader and the player; with
# SHIFT_GAME_ROOT also the R34's loops over the player's files.
add_executable(audio_tests audio_tests.cpp)
target_link_libraries(audio_tests PRIVATE shift_audio)
add_test(NAME audio_tests COMMAND audio_tests)

add_executable(data_tests data_tests.cpp)
target_link_libraries(data_tests PRIVATE shift_data)
add_test(NAME data_tests COMMAND data_tests)

# Counts over the player's own files (FORMAT-CARS Tests). Skipped (exit 77)
# unless SHIFT_GAME_ROOT names the disc and, for the DLC counts,
# SHIFT_STATE_ROOT the launcher state root with the installed packages.
add_test(NAME car_corpus_counts COMMAND car-corpus --check)
set_tests_properties(car_corpus_counts PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 1800 LABELS corpus)

# The stand-in world over the player's Colorado (skipped, exit 77, without
# SHIFT_GAME_ROOT). The first run builds a model index in the temp folder.
add_executable(world_tests world_tests.cpp)
target_link_libraries(world_tests PRIVATE shift_world)
add_test(NAME world_tests COMMAND world_tests)
set_tests_properties(world_tests PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600 LABELS corpus)

# Colorado's collision census (COLLISION.md Tests) over the player's files
# (skipped, exit 77, without SHIFT_GAME_ROOT).
add_executable(collision_tests collision_tests.cpp)
target_link_libraries(collision_tests PRIVATE shift_world)
add_test(NAME collision_tests COMMAND collision_tests)
set_tests_properties(collision_tests PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900 LABELS corpus)

# SPEC-CAR-PHYSICS on the R34's rows: unit checks and scenarios A, B and the
# turn circle on the analytic ground (skipped, exit 77, without
# SHIFT_GAME_ROOT).
add_executable(physics_tests physics_tests.cpp)
target_link_libraries(physics_tests PRIVATE shift_drive)
add_test(NAME physics_tests COMMAND physics_tests)
set_tests_properties(physics_tests PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300 LABELS corpus)

# SPEC-CAMERAS: the five driving cameras of two cars parked, from the
# player's files (skipped, exit 77, without SHIFT_GAME_ROOT).
add_executable(camera_tests camera_tests.cpp)
target_link_libraries(camera_tests PRIVATE shift_drive)
add_test(NAME camera_tests COMMAND camera_tests)
set_tests_properties(camera_tests PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300 LABELS corpus)

# SPEC-TRAFFIC: the road map, the settings and the time-of-day factor over
# the player's files (skipped, exit 77, without SHIFT_GAME_ROOT).
add_executable(traffic_tests traffic_tests.cpp)
target_link_libraries(traffic_tests PRIVATE shift_traffic)
add_test(NAME traffic_tests COMMAND traffic_tests)
set_tests_properties(traffic_tests PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300 LABELS corpus)

# carview's windowed start-up: SDL with video, gamepad and audio under the
# dummy drivers (a build without SDL audio failed only at a player's start).
add_executable(sdl_init_tests sdl_init_tests.cpp)
target_link_libraries(sdl_init_tests PRIVATE SDL3::SDL3-static)
add_test(NAME sdl_init_tests COMMAND sdl_init_tests)

add_executable(handbrake_tests handbrake_tests.cpp)
target_link_libraries(handbrake_tests PRIVATE shift_drive)
add_test(NAME handbrake_tests COMMAND handbrake_tests)
set_tests_properties(handbrake_tests PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120 LABELS corpus)

add_executable(freeroam_tests freeroam_tests.cpp)
target_link_libraries(freeroam_tests PRIVATE shift_freeroam)
add_test(NAME freeroam_tests COMMAND freeroam_tests)

add_executable(club_tests club_tests.cpp)
target_link_libraries(club_tests PRIVATE shift_freeroam)
add_test(NAME club_tests COMMAND club_tests)

add_executable(radio_tests radio_tests.cpp)
target_link_libraries(radio_tests PRIVATE shift_audio)
add_test(NAME radio_tests COMMAND radio_tests)

add_executable(race_tests race_tests.cpp)
target_link_libraries(race_tests PRIVATE shift_race)
add_test(NAME race_tests COMMAND race_tests)

# The Pinyon Shift reference save's counts (FORMAT-PROFILE Tests) over the
# plaintext exports in out/profile (skipped, exit 77, without them).
add_test(NAME profile_import_counts COMMAND profile-import --check ${CMAKE_SOURCE_DIR}/out/profile)
set_tests_properties(profile_import_counts PROPERTIES SKIP_RETURN_CODE 77 LABELS corpus)

add_executable(garage_tests garage_tests.cpp)
target_link_libraries(garage_tests PRIVATE shift_garage)
add_test(NAME garage_tests COMMAND garage_tests)

# The pause menu's settings page (plain C++).
add_executable(settings_tests settings_tests.cpp ../apps/carview/settings_page.cpp)
target_include_directories(settings_tests PRIVATE ../apps/carview)
add_test(NAME settings_tests COMMAND settings_tests)

add_executable(ui_tests ui_tests.cpp ../apps/carview/ui.cpp)
target_include_directories(ui_tests PRIVATE ../apps/carview)
target_link_libraries(ui_tests PRIVATE shift_stb)
add_test(NAME ui_tests COMMAND ui_tests)
