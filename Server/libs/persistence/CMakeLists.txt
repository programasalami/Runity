find_package(asio CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(PostgreSQL REQUIRED)

add_library(runity_persistence STATIC
  src/redis_client.cpp
  src/account_sessions.cpp
  src/characters.cpp
  src/postgres_characters.cpp)
target_include_directories(runity_persistence PUBLIC include)
target_link_libraries(runity_persistence PUBLIC runity_core PRIVATE asio::asio nlohmann_json::nlohmann_json PostgreSQL::PostgreSQL)
runity_set_warnings(runity_persistence)
