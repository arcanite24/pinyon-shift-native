option(SHIFT_RHI_VULKAN "Compile the Vulkan 1.3 reference RHI backend" ON)

set(SHIFT_RHI_SOURCES
  src/rhi_factory.cpp)

set(SHIFT_RHI_USE_VK OFF)
if(SHIFT_RHI_VULKAN AND TARGET shift_volk AND TARGET shift_vma)
  set(SHIFT_RHI_USE_VK ON)
  list(APPEND SHIFT_RHI_SOURCES
    src/rhi_vk_device.cpp
    src/rhi_vk_cmd.cpp
    src/rhi_vk_rt.cpp)
endif()

add_library(shift_rhi STATIC ${SHIFT_RHI_SOURCES})
set_target_properties(shift_rhi PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(shift_rhi PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

if(SHIFT_RHI_USE_VK)
  target_link_libraries(shift_rhi PRIVATE shift_volk shift_vma)
  target_compile_definitions(shift_rhi PRIVATE SHIFT_RHI_VULKAN=1)
  if(SHIFT_ENABLE_RAY_TRACING)
    target_compile_definitions(shift_rhi PRIVATE SHIFT_ENABLE_RAY_TRACING=1)
  endif()
  if(TARGET SDL3::SDL3-static)
    target_link_libraries(shift_rhi PRIVATE SDL3::SDL3-static)
    target_compile_definitions(shift_rhi PRIVATE SHIFT_RHI_HAS_SDL3=1)
  endif()
endif()

if(NOT WIN32)
  find_package(Threads REQUIRED)
  target_link_libraries(shift_rhi PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
endif()

if(MSVC)
  target_compile_options(shift_rhi PRIVATE /W4)
else()
  target_compile_options(shift_rhi PRIVATE -Wall -Wextra)
endif()
