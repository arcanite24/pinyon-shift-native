add_executable(profile-import main.cpp)
target_link_libraries(profile-import PRIVATE shift_profile shift_garage shift_cars)
