add_library(waw_game STATIC src/game_service.cpp)
target_include_directories(waw_game PUBLIC include)
target_link_libraries(waw_game PUBLIC waw_core waw_protocol waw_content waw_sim waw_session waw_persistence)
waw_set_warnings(waw_game)
