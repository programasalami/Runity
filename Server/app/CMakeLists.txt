find_package(nlohmann_json CONFIG REQUIRED)
find_package(asio CONFIG REQUIRED)

add_library(waw_app STATIC config.cpp server_app.cpp)
target_include_directories(waw_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(waw_app PUBLIC waw_core waw_protocol PRIVATE nlohmann_json::nlohmann_json)
waw_set_warnings(waw_app)

add_executable(waw_gameserver main.cpp)
target_link_libraries(waw_gameserver PRIVATE waw_app waw_net waw_session waw_game waw_persistence waw_content asio::asio)
waw_set_warnings(waw_gameserver)
