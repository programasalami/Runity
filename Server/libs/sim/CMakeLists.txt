add_library(runity_sim STATIC src/tile_map.cpp src/movement.cpp src/world.cpp src/behavior/engine.cpp src/rules.cpp src/projectile_path.cpp)
target_include_directories(runity_sim PUBLIC include)
target_link_libraries(runity_sim PUBLIC runity_core runity_content)
runity_set_warnings(runity_sim)
