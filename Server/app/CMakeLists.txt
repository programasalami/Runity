find_package(nlohmann_json CONFIG REQUIRED)
find_package(asio CONFIG REQUIRED)

add_library(runity_app STATIC config.cpp server_app.cpp)
target_include_directories(runity_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(runity_app PUBLIC runity_core runity_protocol PRIVATE nlohmann_json::nlohmann_json)
runity_set_warnings(runity_app)

add_executable(runity_gameserver main.cpp)
# Without --config, a build uses this source tree's config, wherever the program is started from (e.g. by double-clicking it).
target_compile_definitions(runity_gameserver PRIVATE RUNITY_DEFAULT_CONFIG="${PROJECT_SOURCE_DIR}/config/gameserver.json")
target_link_libraries(runity_gameserver PRIVATE runity_app runity_net runity_session runity_game runity_persistence runity_content asio::asio)
runity_set_warnings(runity_gameserver)
