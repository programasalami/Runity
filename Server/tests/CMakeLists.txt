find_package(doctest CONFIG REQUIRED)
find_package(asio CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(PostgreSQL REQUIRED)

add_executable(runity_tests
  test_main.cpp
  test_core.cpp
  test_protocol.cpp
  test_config.cpp
  test_server_app.cpp
  test_net.cpp
  test_persistence.cpp
  test_postgres.cpp
  test_session.cpp
  test_content.cpp
  test_sim.cpp
  test_game.cpp
  test_movement_vectors.cpp
  test_combat.cpp
  test_items.cpp
  test_behavior.cpp
  test_realm.cpp)
target_include_directories(runity_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# doctest forward-declares std types by default, which the current MSVC STL rejects.
target_compile_definitions(runity_tests PRIVATE DOCTEST_CONFIG_USE_STD_HEADERS
  RUNITY_CONTENT_ROOT="${PROJECT_SOURCE_DIR}/../Content"
  RUNITY_PROTOCOL_ROOT="${PROJECT_SOURCE_DIR}/../Protocol"
  RUNITY_REPO_ROOT="${PROJECT_SOURCE_DIR}/..")
target_link_libraries(runity_tests PRIVATE runity_app runity_core runity_protocol runity_net runity_persistence runity_session runity_content runity_sim runity_game asio::asio nlohmann_json::nlohmann_json doctest::doctest PostgreSQL::PostgreSQL)
runity_set_warnings(runity_tests)

add_test(NAME runity_tests COMMAND runity_tests)
