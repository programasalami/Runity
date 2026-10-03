add_library(waw_sim STATIC src/tile_map.cpp src/movement.cpp src/world.cpp src/behavior/engine.cpp src/rules.cpp src/projectile_path.cpp)
target_include_directories(waw_sim PUBLIC include)
target_link_libraries(waw_sim PUBLIC waw_core waw_content)
waw_set_warnings(waw_sim)
