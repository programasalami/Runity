find_package(doctest CONFIG REQUIRED)
find_package(asio CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_executable(waw_tests
  test_main.cpp
  test_core.cpp
  test_protocol.cpp
  test_config.cpp
  test_server_app.cpp
  test_net.cpp
  test_persistence.cpp
  test_session.cpp
  test_content.cpp
  test_sim.cpp
  test_game.cpp
  test_movement_vectors.cpp
  test_combat.cpp
  test_items.cpp
  test_behavior.cpp
  test_realm.cpp)
target_include_directories(waw_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# doctest forward-declares std types by default, which the current MSVC STL rejects.
target_compile_definitions(waw_tests PRIVATE DOCTEST_CONFIG_USE_STD_HEADERS
  WAW_CONTENT_ROOT="${PROJECT_SOURCE_DIR}/../Content"
  WAW_PROTOCOL_ROOT="${PROJECT_SOURCE_DIR}/../Protocol")
target_link_libraries(waw_tests PRIVATE waw_app waw_core waw_protocol waw_net waw_persistence waw_session waw_content waw_sim waw_game asio::asio nlohmann_json::nlohmann_json doctest::doctest)
waw_set_warnings(waw_tests)

add_test(NAME waw_tests COMMAND waw_tests)
