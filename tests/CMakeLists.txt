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

add_executable(wheel_dualsense_tests wheel_dualsense_tests.cpp ../apps/carview/ui.cpp ../apps/carview/ui4_chrome.cpp)
target_include_directories(wheel_dualsense_tests PRIVATE ../apps/carview)
target_link_libraries(wheel_dualsense_tests PRIVATE shift_drive fh1data)
add_test(NAME wheel_dualsense_tests COMMAND wheel_dualsense_tests)

add_executable(camera_telemetry_tests camera_telemetry_tests.cpp ../apps/carview/camera_editor.cpp
               ../apps/carview/ui.cpp ../apps/carview/ui4_chrome.cpp)
target_include_directories(camera_telemetry_tests PRIVATE ../apps/carview)
target_link_libraries(camera_telemetry_tests PRIVATE shift_drive fh1data)
add_test(NAME camera_telemetry_tests COMMAND camera_telemetry_tests)

add_executable(trainer_pacing_tests trainer_pacing_tests.cpp
               ../apps/carview/trainer_overlay.cpp
               ../apps/carview/camera_editor.cpp
               ../apps/carview/autoshow.cpp
               ../apps/carview/upgrade_shop.cpp
               ../apps/carview/hub_scene.cpp)
target_include_directories(trainer_pacing_tests PRIVATE ../apps/carview)
target_link_libraries(trainer_pacing_tests PRIVATE carview_render carview_hud shift_race shift_freeroam shift_garage shift_drive
                      shift_cars shift_stb shift_sqlite3)
add_test(NAME trainer_pacing_tests COMMAND trainer_pacing_tests)

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
target_link_libraries(club_tests PRIVATE shift_freeroam fh1data shift_drive)
add_test(NAME club_tests COMMAND club_tests)

add_executable(radio_tests radio_tests.cpp)
target_link_libraries(radio_tests PRIVATE shift_audio)
add_test(NAME radio_tests COMMAND radio_tests)

add_executable(race_tests race_tests.cpp)
target_link_libraries(race_tests PRIVATE shift_race)
add_test(NAME race_tests COMMAND race_tests)

add_executable(rally_tests rally_tests.cpp)
target_link_libraries(rally_tests PRIVATE shift_race shift_garage shift_audio shift_data shift_drive)
add_test(NAME rally_tests COMMAND rally_tests)

# The Pinyon Shift reference save's counts (FORMAT-PROFILE Tests) over the
# plaintext exports in out/profile (skipped, exit 77, without them).
add_test(NAME profile_import_counts COMMAND profile-import --check ${CMAKE_SOURCE_DIR}/out/profile)
set_tests_properties(profile_import_counts PROPERTIES SKIP_RETURN_CODE 77 LABELS corpus)

add_executable(garage_tests garage_tests.cpp)
target_link_libraries(garage_tests PRIVATE shift_garage)
add_test(NAME garage_tests COMMAND garage_tests)

# The pause menu's settings page (plain C++).
add_executable(settings_tests settings_tests.cpp ../apps/carview/settings_page.cpp ../apps/carview/render_modern.cpp ../apps/carview/rt_logic.cpp)
target_include_directories(settings_tests PRIVATE ../apps/carview)
target_link_libraries(settings_tests PRIVATE shift_drive)
add_test(NAME settings_tests COMMAND settings_tests)

add_executable(ui_tests ui_tests.cpp ../apps/carview/ui.cpp ../apps/carview/ui4_chrome.cpp
               ../apps/carview/events_page.cpp ../apps/carview/hud.cpp)
target_include_directories(ui_tests PRIVATE ../apps/carview)
target_link_libraries(ui_tests PRIVATE carview_hud shift_stb fh1data shift_race shift_garage shift_freeroam)
add_test(NAME ui_tests COMMAND ui_tests)
# Slice 11: render scale / HUD / HDR / upscaler fallbacks / .cube reader / frame stats (no Vulkan).
add_executable(render_modern_tests render_modern_tests.cpp ../apps/carview/render_modern.cpp ../apps/carview/rt_logic.cpp
               ../apps/carview/perf_overlay.cpp ../apps/carview/frame_inspector.cpp
               ../apps/carview/upscaler.cpp ../apps/carview/ui.cpp
               ../apps/carview/platform_support.cpp)
target_include_directories(render_modern_tests PRIVATE ../apps/carview)
target_link_libraries(render_modern_tests PRIVATE shift_stb fh1data SDL3::SDL3-static)
add_test(NAME render_modern_tests COMMAND render_modern_tests)
add_executable(benchmark_tests benchmark_tests.cpp ../apps/carview/benchmark.cpp
               ../apps/carview/perf_overlay.cpp ../apps/carview/ui.cpp)
target_include_directories(benchmark_tests PRIVATE ../apps/carview)
target_link_libraries(benchmark_tests PRIVATE shift_stb fh1data)
add_test(NAME benchmark_tests COMMAND benchmark_tests)
# Slice 11: the GPU-driven world's arena allocator, resident set, draw table and culling rule (no Vulkan).
add_executable(world_gpu_tests world_gpu_tests.cpp ../apps/carview/world_gpu_logic.cpp)
target_include_directories(world_gpu_tests PRIVATE ../apps/carview)
add_test(NAME world_gpu_tests COMMAND world_gpu_tests)

# Hardware Ray Tracing & Path Tracing capability negotiation, tier clamping, BVH masks, BLAS LRU & settings (no Vulkan).
add_executable(rt_logic_tests rt_logic_tests.cpp ../apps/carview/rt_logic.cpp ../apps/carview/render_modern.cpp
               ../apps/carview/settings_page.cpp)
target_include_directories(rt_logic_tests PRIVATE ../apps/carview)
target_link_libraries(rt_logic_tests PRIVATE shift_drive)
add_test(NAME rt_logic_tests COMMAND rt_logic_tests)

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

add_executable(world_polish_tests world_polish_tests.cpp ../apps/carview/settings_page.cpp
               ../apps/carview/render_modern.cpp ../apps/carview/rt_logic.cpp)
target_include_directories(world_polish_tests PRIVATE ../apps/carview)
target_link_libraries(world_polish_tests PRIVATE shift_world shift_drive)
add_test(NAME world_polish_tests COMMAND world_polish_tests)

add_executable(lamps_tests lamps_tests.cpp)
target_link_libraries(lamps_tests PRIVATE shift_drive fh1data)
add_test(NAME lamps_tests COMMAND lamps_tests)

add_executable(gps_tests gps_tests.cpp ../apps/carview/settings_page.cpp
               ../apps/carview/render_modern.cpp ../apps/carview/rt_logic.cpp)
target_include_directories(gps_tests PRIVATE ../apps/carview)
target_link_libraries(gps_tests PRIVATE shift_traffic shift_audio shift_drive)
add_test(NAME gps_tests COMMAND gps_tests)

add_executable(road_discovery_tests road_discovery_tests.cpp ../apps/carview/map_view.cpp
               ../apps/carview/settings_page.cpp ../apps/carview/render_modern.cpp ../apps/carview/rt_logic.cpp)
target_include_directories(road_discovery_tests PRIVATE ../apps/carview)
target_link_libraries(road_discovery_tests PRIVATE shift_freeroam shift_traffic shift_profile shift_drive fh1data shift_audio)
add_test(NAME road_discovery_tests COMMAND road_discovery_tests)

add_executable(outpost_tests outpost_tests.cpp)
target_link_libraries(outpost_tests PRIVATE shift_freeroam shift_drive fh1data shift_audio)
add_test(NAME outpost_tests COMMAND outpost_tests)

add_executable(pi_tuning_tests pi_tuning_tests.cpp)
target_include_directories(pi_tuning_tests PRIVATE ../libs/race/include)
target_link_libraries(pi_tuning_tests PRIVATE shift_garage shift_drive shift_cars shift_sqlite3)
add_test(NAME pi_tuning_tests COMMAND pi_tuning_tests)

add_executable(visual_upgrades_tests visual_upgrades_tests.cpp)
target_link_libraries(visual_upgrades_tests PRIVATE shift_cars)
add_test(NAME visual_upgrades_tests COMMAND visual_upgrades_tests)

add_executable(hub_shop_tests hub_shop_tests.cpp
  ../apps/carview/autoshow.cpp
  ../apps/carview/upgrade_shop.cpp
  ../apps/carview/hub_scene.cpp)
target_include_directories(hub_shop_tests PRIVATE ../apps/carview)
target_link_libraries(hub_shop_tests PRIVATE carview_render carview_hud shift_freeroam shift_garage shift_race shift_drive shift_cars shift_sqlite3)
add_test(NAME hub_shop_tests COMMAND hub_shop_tests)

add_executable(livery_damage_tests livery_damage_tests.cpp
  ../apps/carview/autoshow.cpp
  ../apps/carview/upgrade_shop.cpp
  ../apps/carview/hub_scene.cpp
  ../apps/carview/livery_editor.cpp
  ../apps/carview/settings_page.cpp)
target_include_directories(livery_damage_tests PRIVATE ../apps/carview)
target_link_libraries(livery_damage_tests PRIVATE carview_render carview_hud shift_freeroam shift_garage shift_race shift_drive shift_cars shift_sqlite3 shift_stb)
add_test(NAME livery_damage_tests COMMAND livery_damage_tests)

add_executable(acoustics_vo_radio_tests acoustics_vo_radio_tests.cpp)
target_link_libraries(acoustics_vo_radio_tests PRIVATE shift_audio fh1data)
add_test(NAME acoustics_vo_radio_tests COMMAND acoustics_vo_radio_tests)

# Slice 13A: the modding platform (a sample native mod for the plugin test).
add_library(shift_test_mod_plugin MODULE mod_plugin_sample.c)
target_include_directories(shift_test_mod_plugin PRIVATE ../libs/cars/include)
add_executable(mods_tests mods_tests.cpp)
target_link_libraries(mods_tests PRIVATE shift_cars shift_sqlite3 fh1data)
if(TARGET shift_stb)
  target_link_libraries(mods_tests PRIVATE shift_stb)
endif()
add_dependencies(mods_tests shift_test_mod_plugin)
target_compile_definitions(mods_tests PRIVATE SHIFT_TEST_PLUGIN="$<TARGET_FILE:shift_test_mod_plugin>")
add_test(NAME mods_tests COMMAND mods_tests)
set_tests_properties(mods_tests PROPERTIES TIMEOUT 300)

add_executable(save_ecosystem_tests save_ecosystem_tests.cpp)
target_link_libraries(save_ecosystem_tests PRIVATE shift_profile shift_drive shift_garage fh1data shift_sqlite3)
add_test(NAME save_ecosystem_tests COMMAND save_ecosystem_tests)

if(SHIFT_BUILD_CARVIEW)
  add_executable(mods_saves_ui_tests mods_saves_ui_tests.cpp
    ../apps/carview/mods_page.cpp
    ../apps/carview/save_sync_ui.cpp
    ../apps/carview/ui_widgets.cpp
    ../apps/carview/events_page.cpp
    ../apps/carview/settings_page.cpp)
  target_include_directories(mods_saves_ui_tests PRIVATE ../apps/carview)
  target_link_libraries(mods_saves_ui_tests PRIVATE carview_render carview_hud shift_cars shift_profile shift_freeroam shift_race
                        shift_garage shift_drive shift_stb fh1data shift_sqlite3)
  add_test(NAME mods_saves_ui_tests COMMAND mods_saves_ui_tests)
endif()

if(SHIFT_BUILD_CARVIEW)
  add_executable(fmv_story_tests fmv_story_tests.cpp
    ../apps/carview/fmv_player.cpp
    ../apps/carview/story_intro.cpp
    ../apps/carview/settings.cpp
    ../apps/carview/settings_page.cpp)
  target_include_directories(fmv_story_tests PRIVATE ../apps/carview)
  target_link_libraries(fmv_story_tests PRIVATE carview_render shift_audio shift_drive)
  add_test(NAME fmv_story_tests COMMAND fmv_story_tests)

  add_executable(campaign_flow_tests campaign_flow_tests.cpp
    ../apps/carview/campaign_director.cpp
    ../apps/carview/campaign_ui.cpp
    ../apps/carview/audio_host.cpp
    ../apps/carview/fmv_player.cpp)
  target_include_directories(campaign_flow_tests PRIVATE ../apps/carview)
  target_link_libraries(campaign_flow_tests PRIVATE carview_render shift_race shift_audio shift_drive shift_cars)
  add_test(NAME campaign_flow_tests COMMAND campaign_flow_tests)

  add_executable(ui4_cjk_l10n_tests ui4_cjk_l10n_tests.cpp
    ../apps/carview/ui_widgets.cpp
    ../apps/carview/events_page.cpp
    ../apps/carview/settings_page.cpp
    ../apps/carview/fmv_player.cpp
    ../apps/carview/story_intro.cpp)
  target_include_directories(ui4_cjk_l10n_tests PRIVATE ../apps/carview)
  target_link_libraries(ui4_cjk_l10n_tests PRIVATE carview_render carview_hud shift_freeroam shift_race shift_garage shift_drive
                        shift_cars shift_audio shift_stb)
  add_test(NAME ui4_cjk_l10n_tests COMMAND ui4_cjk_l10n_tests)

  add_executable(ui4_parity_tests ui4_parity_tests.cpp
    ../apps/carview/ui_widgets.cpp
    ../apps/carview/map_view.cpp
    ../apps/carview/events_page.cpp
    ../apps/carview/autoshow.cpp
    ../apps/carview/upgrade_shop.cpp
    ../apps/carview/hub_scene.cpp
    ../apps/carview/livery_editor.cpp
    ../apps/carview/photo_mode.cpp
    ../apps/carview/replay_view.cpp
    ../apps/carview/achievements.cpp
    ../apps/carview/mods_page.cpp
    ../apps/carview/save_sync_ui.cpp
    ../apps/carview/club_view.cpp
    ../apps/carview/camera_editor.cpp
    ../apps/carview/trainer_overlay.cpp
    ../apps/carview/settings_page.cpp)
  target_include_directories(ui4_parity_tests PRIVATE ../apps/carview)
  target_link_libraries(ui4_parity_tests PRIVATE carview_render carview_hud shift_freeroam shift_traffic shift_race shift_garage
                        shift_drive shift_cars shift_audio shift_profile shift_stb shift_sqlite3)
  add_test(NAME ui4_parity_tests COMMAND ui4_parity_tests)
endif()

add_executable(photo_mode_tests photo_mode_tests.cpp ../apps/carview/photo_mode.cpp ../apps/carview/ui.cpp
               ../apps/carview/ui4_chrome.cpp ../apps/carview/settings_page.cpp
               ../apps/carview/render_modern.cpp ../apps/carview/rt_logic.cpp)
target_include_directories(photo_mode_tests PRIVATE ../apps/carview)
target_link_libraries(photo_mode_tests PRIVATE shift_drive shift_stb fh1data shift_volk)
add_test(NAME photo_mode_tests COMMAND photo_mode_tests)

add_executable(replay_tests replay_tests.cpp ../apps/carview/replay_view.cpp ../apps/carview/ui.cpp
               ../apps/carview/ui4_chrome.cpp)
target_include_directories(replay_tests PRIVATE ../apps/carview)
target_link_libraries(replay_tests PRIVATE shift_drive fh1data)
add_test(NAME replay_tests COMMAND replay_tests)

add_executable(achievements_tests achievements_tests.cpp ../apps/carview/achievements.cpp ../apps/carview/ui.cpp
               ../apps/carview/ui4_chrome.cpp)
target_include_directories(achievements_tests PRIVATE ../apps/carview)
target_link_libraries(achievements_tests PRIVATE carview_hud fh1data shift_drive shift_freeroam shift_stb shift_race shift_garage)
add_test(NAME achievements_tests COMMAND achievements_tests)

if(SHIFT_BUILD_CARVIEW)
  add_executable(showroom_vram_tests showroom_vram_tests.cpp)
  target_link_libraries(showroom_vram_tests PRIVATE carview_render)
  add_test(NAME showroom_vram_tests COMMAND showroom_vram_tests)
  set_tests_properties(showroom_vram_tests PROPERTIES SKIP_RETURN_CODE 77)

  add_executable(platform_touch_tests platform_touch_tests.cpp)
  target_link_libraries(platform_touch_tests PRIVATE carview_render)
  add_test(NAME platform_touch_tests COMMAND platform_touch_tests)

  add_executable(playability_fixes_tests playability_fixes_tests.cpp)
  target_link_libraries(playability_fixes_tests PRIVATE carview_render shift_drive shift_world)
  add_test(NAME playability_fixes_tests COMMAND playability_fixes_tests)

  add_executable(rhi_shader_compat_tests rhi_shader_compat_tests.cpp)
  target_link_libraries(rhi_shader_compat_tests PRIVATE carview_render)
  target_compile_definitions(rhi_shader_compat_tests PRIVATE SHIFT_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  add_test(NAME rhi_shader_compat_tests COMMAND rhi_shader_compat_tests)
endif()

if(SHIFT_BUILD_LAUNCHER)
  add_executable(launcher_tests launcher_tests.cpp)
  target_link_libraries(launcher_tests PRIVATE shift_launcher_core)
  add_test(NAME launcher_tests COMMAND launcher_tests)
endif()

add_executable(rhi_tests rhi_tests.cpp)
target_link_libraries(rhi_tests PRIVATE shift_rhi)
if(MSVC)
  target_compile_options(rhi_tests PRIVATE /W4)
else()
  target_compile_options(rhi_tests PRIVATE -Wall -Wextra)
endif()
add_test(NAME rhi_tests COMMAND rhi_tests)

add_executable(android_standalone_tests android_standalone_tests.cpp)
target_link_libraries(android_standalone_tests PRIVATE shift_setup)
add_test(NAME android_standalone_tests COMMAND android_standalone_tests)
