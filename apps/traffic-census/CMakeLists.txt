add_executable(traffic-census main.cpp)
target_link_libraries(traffic-census PRIVATE shift_traffic shift_world shift_cars)
