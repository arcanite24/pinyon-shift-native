# Shaders: compiled from GLSL when a compiler is found, else the committed
# SPIR-V in shaders/prebuilt (tools/regenerate-shaders.py refreshes it).
set(SHIFT_GLSL_COMPILER "" CACHE FILEPATH "glslangValidator or glslc (optional)")
if(NOT SHIFT_GLSL_COMPILER)
  find_program(SHIFT_GLSL_FOUND NAMES glslangValidator glslc
    HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
  if(SHIFT_GLSL_FOUND)
    set(SHIFT_GLSL_COMPILER "${SHIFT_GLSL_FOUND}")
  endif()
endif()

set(shader_dir "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
set(generated "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${generated}")
set(shader_headers "")
set(carview_shaders car.vert car.frag showroom.vert showroom.frag mask.frag world.vert world.frag world_opaque.frag world_gpu.vert world_gpu.frag world_gpu_opaque.frag world_tree_gpu.vert world_cull.comp tree_cull.comp hiz_gen.comp post.vert post.frag bloom_down.frag bloom_up.frag ssao.frag sscs.frag ssr.frag volumetric.frag fxaa.frag upscale.frag motion_vectors.frag mblur_tile.frag temporal_upscale.frag inspector_preview.frag hdr_output.frag shadow.frag ui.vert ui.frag sky.vert sky.frag stars.vert stars.frag glow.vert glow.frag particle.vert particle.frag skid.vert skid.frag env_prefilter.frag contact_shadow.vert contact_shadow.frag)
if(SHIFT_ENABLE_RAY_TRACING)
  list(APPEND carview_shaders rt_reflections.comp rt_shadows.comp rt_ao.comp rt_pathtrace.comp rt_svgf_temporal.comp rt_svgf_atrous.comp rt_composite.comp)
endif()
foreach(shader ${carview_shaders})
  string(REPLACE "." "_" id "${shader}")
  string(SUBSTRING "${id}" 0 1 first)
  string(TOUPPER "${first}" first)
  string(SUBSTRING "${id}" 1 -1 rest)
  set(source "${shader_dir}/${shader}")
  set(header "${generated}/${id}_spv.h")
  if(SHIFT_GLSL_COMPILER)
    set(spv "${generated}/${shader}.spv")
    get_filename_component(compiler_name "${SHIFT_GLSL_COMPILER}" NAME_WE)
    if(compiler_name STREQUAL "glslc")
      set(compile_args --target-env=vulkan1.3 -o "${spv}" "${source}")
    else()
      set(compile_args -V --target-env vulkan1.3 -o "${spv}" "${source}")
    endif()
    add_custom_command(OUTPUT "${spv}"
      COMMAND "${SHIFT_GLSL_COMPILER}" ${compile_args}
      DEPENDS "${source}" "${shader_dir}/world_vert.glsl" "${shader_dir}/world_frag.glsl" "${shader_dir}/rt_common.glsl" VERBATIM)
  else()
    set(spv "${shader_dir}/prebuilt/${shader}.spv")
  endif()
  add_custom_command(OUTPUT "${header}"
    COMMAND "${CMAKE_COMMAND}" -DINPUT=${spv} -DOUTPUT=${header} -DNAME=k${first}${rest}_spv
            -P "${PROJECT_SOURCE_DIR}/cmake/EmbedSpirv.cmake"
    DEPENDS "${spv}" "${PROJECT_SOURCE_DIR}/cmake/EmbedSpirv.cmake" VERBATIM)
  list(APPEND shader_headers "${header}")
endforeach()
if(SHIFT_GLSL_COMPILER)
  message(STATUS "carview shaders: compiled with ${SHIFT_GLSL_COMPILER}")
else()
  message(STATUS "carview shaders: no GLSL compiler found, using shaders/prebuilt")
endif()

set(SHIFT_GIT_COMMIT "unknown")
find_package(Git QUIET)
if(GIT_FOUND)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    OUTPUT_VARIABLE _shift_git_rev
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
  if(_shift_git_rev)
    set(SHIFT_GIT_COMMIT "${_shift_git_rev}")
  endif()
endif()

add_library(carview_render STATIC
  benchmark.cpp
  renderer_device.cpp
  renderer_resources.cpp
  renderer_modern_setup.cpp
  renderer_modern_record.cpp
  renderer_post.cpp
  renderer_frame.cpp
  renderer_ui.cpp
  renderer_swapchain.cpp
  showroom_car_load.cpp
  showroom_car_state.cpp
  showroom_pipelines.cpp
  showroom_upload.cpp
  showroom_draw.cpp
  showroom_shadows.cpp
  showroom_env.cpp
  showroom_sky.cpp
  showroom_lights.cpp
  showroom_world_stream.cpp
  showroom_world_gpu.cpp
  showroom_vfx.cpp
  showroom_actors.cpp
  ui.cpp
  ui4_chrome.cpp
  platform_support.cpp
  gamepad_db.cpp
  touch_controls.cpp
  render_modern.cpp
  rt_logic.cpp
  rt_scene.cpp
  rt_pipeline.cpp
  upscaler.cpp
  frame_inspector.cpp
  perf_overlay.cpp
  world_gpu_logic.cpp
  world_gpu.cpp
  ${shader_headers})
set_target_properties(carview_render PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(carview_render PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${generated}")
target_compile_definitions(carview_render PUBLIC
  SHIFT_GIT_COMMIT="${SHIFT_GIT_COMMIT}"
  SHIFT_BUILD_CONFIG="$<IF:$<BOOL:$<CONFIG>>,$<CONFIG>,${CMAKE_BUILD_TYPE}>"
  $<$<BOOL:${SHIFT_ENABLE_RAY_TRACING}>:SHIFT_ENABLE_RAY_TRACING=1>)
target_link_libraries(carview_render PUBLIC fh1data shift_cars shift_drive shift_world shift_rhi shift_volk shift_vma SDL3::SDL3-static)
if(WIN32)
  target_link_libraries(carview_render PUBLIC mfplat mfreadwrite mfuuid ole32)
else()
  find_package(Threads REQUIRED)
  target_link_libraries(carview_render PUBLIC Threads::Threads)
endif()

add_library(carview_hud STATIC
  game_hud_core.cpp
  game_hud_race.cpp
  game_hud_freeroam.cpp
  game_hud_menus.cpp
  help_bar_screen.cpp
  hud_scene.cpp
  hud_screen.cpp
  pause_screen.cpp
  rally_hub.cpp
  scene_screen.cpp)
set_target_properties(carview_hud PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(carview_hud PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(carview_hud PUBLIC fh1data shift_freeroam shift_race shift_garage shift_drive)

set(carview_sources
  main.cpp
  car_model.cpp
  hud.cpp
  process_time.cpp
  reference.cpp
  showroom_mode.cpp
  drive_helpers_runtime.cpp
  drive_camera_input.cpp
  drive_cli.cpp
  drive_app.cpp
  drive_world_host.cpp
  drive_session.cpp
  drive_club.cpp
  drive_simulation.cpp
  drive_world_sync.cpp
  drive_hud_sync.cpp
  drive_map_radio.cpp
  drive_menus_dispatch.cpp
  drive_benchmark_runner.cpp
  drive_headless_runner.cpp
  drive_input_world_screens.cpp
  drive_input_hub_screens.cpp
  drive_input_overlay_screens.cpp
  drive_input_settings_media.cpp
  drive_input_loop.cpp
  drive_render_loop.cpp
  drive_frame_render.cpp
  showroom_main.cpp
  contact_log.cpp
  safe_reset.cpp
  traffic_view.cpp
  race_view.cpp
  world_view.cpp
  map_view.cpp
  upgrade_shop.cpp
  autoshow.cpp
  hub_scene.cpp
  livery_editor.cpp
  career_page.cpp
  club_view.cpp
  events_page.cpp
  settings.cpp
  settings_page.cpp
  camera_editor.cpp
  trainer_overlay.cpp
  photo_mode.cpp
  playtest_recorder.cpp
  replay_view.cpp
  achievements.cpp
  audio_host.cpp
  campaign_director.cpp
  first_time_career_ui.cpp
  drive_first_time_career.cpp
  cutscene_runtime.cpp
  campaign_ui.cpp
  fmv_player.cpp
  story_intro.cpp
  mods_page.cpp
  save_sync_ui.cpp
  ui_widgets.cpp
  display_hdr_win.cpp)
# Android (M7): libmain.so for SDL's activity (tools/android-apk.py makes the
# APK); SHIFT_ANDROID_EXECUTABLE keeps the executable run over adb.
option(SHIFT_ANDROID_EXECUTABLE "Android: build carview as an executable, not the app's libmain.so" OFF)
if(ANDROID AND NOT SHIFT_ANDROID_EXECUTABLE)
  add_library(carview SHARED ${carview_sources} android/jni/android_setup_jni.cpp)
  set_target_properties(carview PROPERTIES OUTPUT_NAME main)
  target_link_libraries(carview PRIVATE android log shift_setup)
else()
  add_executable(carview ${carview_sources})
  if(APPLE)
    set_target_properties(carview PROPERTIES
      BUILD_RPATH "@executable_path/../Frameworks"
      INSTALL_RPATH "@executable_path/../Frameworks")
  endif()
endif()
target_link_libraries(carview PRIVATE carview_render carview_hud shift_drive shift_world shift_audio shift_traffic shift_freeroam shift_race shift_cutscene shift_profile shift_garage shift_stb shift_sqlite3)
if(EXISTS "${PROJECT_SOURCE_DIR}/config/gamecontrollerdb.txt" AND NOT (ANDROID AND NOT SHIFT_ANDROID_EXECUTABLE))
  add_custom_command(TARGET carview POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${PROJECT_SOURCE_DIR}/config/gamecontrollerdb.txt"
      "$<TARGET_FILE_DIR:carview>/gamecontrollerdb.txt")
endif()
