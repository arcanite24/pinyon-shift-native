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
foreach(stage vert frag)
  set(source "${shader_dir}/car.${stage}")
  set(header "${generated}/car_${stage}_spv.h")
  if(SHIFT_GLSL_COMPILER)
    set(spv "${generated}/car.${stage}.spv")
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
    set(spv "${shader_dir}/prebuilt/car.${stage}.spv")
  endif()
  add_custom_command(OUTPUT "${header}"
    COMMAND "${CMAKE_COMMAND}" -DINPUT=${spv} -DOUTPUT=${header} -DNAME=kCar_${stage}_spv
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
  renderer.cpp
  world_view.cpp
  ${shader_headers})
target_include_directories(carview PRIVATE "${generated}")
target_link_libraries(carview PRIVATE fh1data shift_volk SDL3::SDL3-static shift_stb)
if(NOT WIN32)
  find_package(Threads REQUIRED)
  target_link_libraries(carview PRIVATE Threads::Threads)
endif()
