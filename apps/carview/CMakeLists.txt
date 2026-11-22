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
foreach(shader car.vert car.frag showroom.vert showroom.frag mask.frag world.vert world.frag post.vert post.frag shadow.frag ui.vert ui.frag)
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
      DEPENDS "${source}" VERBATIM)
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

add_library(carview_render STATIC
  renderer.cpp
  showroom.cpp
  ui.cpp
  ${shader_headers})
set_target_properties(carview_render PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(carview_render PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${generated}")
target_link_libraries(carview_render PUBLIC fh1data shift_cars shift_volk shift_vma SDL3::SDL3-static)
if(NOT WIN32)
  find_package(Threads REQUIRED)
  target_link_libraries(carview_render PUBLIC Threads::Threads)
endif()

set(carview_sources
  main.cpp
  car_model.cpp
  hud.cpp
  game_hud.cpp
  hud_scene.cpp
  process_time.cpp
  reference.cpp
  showroom_main.cpp
  contact_log.cpp
  safe_reset.cpp
  traffic_view.cpp
  race_view.cpp
  world_view.cpp
  map_view.cpp
  upgrade_shop.cpp
  autoshow.cpp
  career_page.cpp
  events_page.cpp
  settings.cpp
  settings_page.cpp
  audio_host.cpp)
# Android (M7): libmain.so for SDL's activity (tools/android-apk.py makes the
# APK); SHIFT_ANDROID_EXECUTABLE keeps the executable run over adb.
option(SHIFT_ANDROID_EXECUTABLE "Android: build carview as an executable, not the app's libmain.so" OFF)
if(ANDROID AND NOT SHIFT_ANDROID_EXECUTABLE)
  add_library(carview SHARED ${carview_sources})
  set_target_properties(carview PROPERTIES OUTPUT_NAME main)
  target_link_libraries(carview PRIVATE android log)
else()
  add_executable(carview ${carview_sources})
endif()
target_link_libraries(carview PRIVATE carview_render shift_drive shift_world shift_audio shift_traffic shift_freeroam shift_race shift_profile shift_garage shift_stb)
