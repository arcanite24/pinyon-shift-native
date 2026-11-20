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
foreach(shader car.vert car.frag showroom.vert showroom.frag mask.frag world.vert world.frag)
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

add_executable(carview
  main.cpp
  car_model.cpp
  hud.cpp
  process_time.cpp
  renderer.cpp
  reference.cpp
  showroom.cpp
  showroom_main.cpp
  world_view.cpp
  ${shader_headers})
target_include_directories(carview PRIVATE "${generated}")
target_link_libraries(carview PRIVATE fh1data shift_cars shift_drive shift_world shift_audio shift_volk SDL3::SDL3-static shift_stb)
if(NOT WIN32)
  find_package(Threads REQUIRED)
  target_link_libraries(carview PRIVATE Threads::Threads)
endif()
