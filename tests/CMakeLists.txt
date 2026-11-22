# carview's reference reader and metric are plain C++ with no Vulkan.
add_executable(fh1data_tests fh1data_tests.cpp ../apps/carview/reference.cpp)
target_include_directories(fh1data_tests PRIVATE ../apps/carview)
target_link_libraries(fh1data_tests PRIVATE fh1data shift_cars)
add_test(NAME fh1data_tests COMMAND fh1data_tests)

add_executable(drive_tests drive_tests.cpp)
target_link_libraries(drive_tests PRIVATE shift_drive)
add_test(NAME drive_tests COMMAND drive_tests)

add_executable(rewind_tests rewind_tests.cpp)
target_link_libraries(rewind_tests PRIVATE shift_drive)
add_test(NAME rewind_tests COMMAND rewind_tests)

add_executable(driving_line_tests driving_line_tests.cpp)
target_link_libraries(driving_line_tests PRIVATE shift_drive)
add_test(NAME driving_line_tests COMMAND driving_line_tests)

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

add_executable(rumble_tests rumble_tests.cpp)
target_link_libraries(rumble_tests PRIVATE shift_drive)
add_test(NAME rumble_tests COMMAND rumble_tests)

add_executable(controls_tests controls_tests.cpp)
target_link_libraries(controls_tests PRIVATE shift_drive)
add_test(NAME controls_tests COMMAND controls_tests)

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
target_link_libraries(settings_tests PRIVATE shift_drive)
add_test(NAME settings_tests COMMAND settings_tests)

add_executable(ui_tests ui_tests.cpp ../apps/carview/ui.cpp ../apps/carview/game_hud.cpp ../apps/carview/hud_scene.cpp
               ../apps/carview/events_page.cpp ../apps/carview/hud.cpp)
target_include_directories(ui_tests PRIVATE ../apps/carview)
target_link_libraries(ui_tests PRIVATE shift_stb fh1data shift_race shift_garage shift_freeroam)
add_test(NAME ui_tests COMMAND ui_tests)

add_executable(career_tests career_tests.cpp)
target_link_libraries(career_tests PRIVATE shift_race shift_freeroam shift_drive shift_data)
add_test(NAME career_tests COMMAND career_tests)

add_executable(carview_drive_tests carview_drive_tests.cpp)
target_include_directories(carview_drive_tests PRIVATE ../apps/carview)
target_link_libraries(carview_drive_tests PRIVATE shift_world shift_drive shift_audio)
add_test(NAME carview_drive_tests COMMAND carview_drive_tests)

add_executable(world_texture_lru_tests world_texture_lru_tests.cpp)
target_include_directories(world_texture_lru_tests PRIVATE ../apps/carview)
add_test(NAME world_texture_lru_tests COMMAND world_texture_lru_tests)

add_executable(tod_tests tod_tests.cpp)
target_link_libraries(tod_tests PRIVATE shift_world)
add_test(NAME tod_tests COMMAND tod_tests)

add_executable(vfx_tests vfx_tests.cpp)
target_link_libraries(vfx_tests PRIVATE shift_world)
add_test(NAME vfx_tests COMMAND vfx_tests)

add_executable(smashables_tests smashables_tests.cpp)
target_link_libraries(smashables_tests PRIVATE shift_world)
add_test(NAME smashables_tests COMMAND smashables_tests)

add_executable(world_polish_tests world_polish_tests.cpp ../apps/carview/settings_page.cpp)
target_include_directories(world_polish_tests PRIVATE ../apps/carview)
target_link_libraries(world_polish_tests PRIVATE shift_world shift_drive)
add_test(NAME world_polish_tests COMMAND world_polish_tests)

add_executable(lamps_tests lamps_tests.cpp)
target_link_libraries(lamps_tests PRIVATE shift_drive fh1data)
add_test(NAME lamps_tests COMMAND lamps_tests)

add_executable(gps_tests gps_tests.cpp ../apps/carview/settings_page.cpp)
target_include_directories(gps_tests PRIVATE ../apps/carview)
target_link_libraries(gps_tests PRIVATE shift_traffic shift_audio shift_drive)
add_test(NAME gps_tests COMMAND gps_tests)

add_executable(road_discovery_tests road_discovery_tests.cpp ../apps/carview/map_view.cpp ../apps/carview/settings_page.cpp)
target_include_directories(road_discovery_tests PRIVATE ../apps/carview)
target_link_libraries(road_discovery_tests PRIVATE shift_freeroam shift_traffic shift_profile shift_drive fh1data shift_audio)
add_test(NAME road_discovery_tests COMMAND road_discovery_tests)

add_executable(outpost_tests outpost_tests.cpp)
target_link_libraries(outpost_tests PRIVATE shift_freeroam shift_drive fh1data shift_audio)
add_test(NAME outpost_tests COMMAND outpost_tests)

add_executable(pi_tuning_tests pi_tuning_tests.cpp)
target_link_libraries(pi_tuning_tests PRIVATE shift_garage shift_drive shift_cars shift_sqlite3)
add_test(NAME pi_tuning_tests COMMAND pi_tuning_tests)

add_executable(visual_upgrades_tests visual_upgrades_tests.cpp)
target_link_libraries(visual_upgrades_tests PRIVATE shift_cars)
add_test(NAME visual_upgrades_tests COMMAND visual_upgrades_tests)

add_executable(hub_shop_tests hub_shop_tests.cpp
  ../apps/carview/autoshow.cpp
  ../apps/carview/upgrade_shop.cpp
  ../apps/carview/hub_scene.cpp
  ../apps/carview/game_hud.cpp
  ../apps/carview/hud_scene.cpp)
target_include_directories(hub_shop_tests PRIVATE ../apps/carview)
target_link_libraries(hub_shop_tests PRIVATE carview_render shift_freeroam shift_garage shift_drive shift_cars shift_sqlite3)
add_test(NAME hub_shop_tests COMMAND hub_shop_tests)

add_executable(livery_damage_tests livery_damage_tests.cpp
  ../apps/carview/autoshow.cpp
  ../apps/carview/upgrade_shop.cpp
  ../apps/carview/hub_scene.cpp
  ../apps/carview/livery_editor.cpp
  ../apps/carview/settings_page.cpp
  ../apps/carview/game_hud.cpp
  ../apps/carview/hud_scene.cpp)
target_include_directories(livery_damage_tests PRIVATE ../apps/carview)
target_link_libraries(livery_damage_tests PRIVATE carview_render shift_freeroam shift_garage shift_drive shift_cars shift_sqlite3 shift_stb)
add_test(NAME livery_damage_tests COMMAND livery_damage_tests)

add_executable(acoustics_vo_radio_tests acoustics_vo_radio_tests.cpp)
target_link_libraries(acoustics_vo_radio_tests PRIVATE shift_audio fh1data)
add_test(NAME acoustics_vo_radio_tests COMMAND acoustics_vo_radio_tests)

if(SHIFT_BUILD_CARVIEW)
  add_executable(fmv_story_tests fmv_story_tests.cpp
    ../apps/carview/fmv_player.cpp
    ../apps/carview/story_intro.cpp
    ../apps/carview/settings.cpp
    ../apps/carview/settings_page.cpp)
  target_include_directories(fmv_story_tests PRIVATE ../apps/carview)
  target_link_libraries(fmv_story_tests PRIVATE carview_render shift_audio shift_drive)
  add_test(NAME fmv_story_tests COMMAND fmv_story_tests)

  add_executable(ui4_cjk_l10n_tests ui4_cjk_l10n_tests.cpp
    ../apps/carview/ui_widgets.cpp
    ../apps/carview/hud_scene.cpp
    ../apps/carview/game_hud.cpp
    ../apps/carview/fmv_player.cpp
    ../apps/carview/story_intro.cpp)
  target_include_directories(ui4_cjk_l10n_tests PRIVATE ../apps/carview)
  target_link_libraries(ui4_cjk_l10n_tests PRIVATE carview_render shift_freeroam shift_cars shift_audio shift_stb)
  add_test(NAME ui4_cjk_l10n_tests COMMAND ui4_cjk_l10n_tests)
endif()

if(SHIFT_BUILD_CARVIEW)
  add_executable(showroom_vram_tests showroom_vram_tests.cpp)
  target_link_libraries(showroom_vram_tests PRIVATE carview_render)
  add_test(NAME showroom_vram_tests COMMAND showroom_vram_tests)
  set_tests_properties(showroom_vram_tests PROPERTIES SKIP_RETURN_CODE 77)
endif()
