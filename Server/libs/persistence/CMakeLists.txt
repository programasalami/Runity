find_package(asio CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_library(waw_persistence STATIC
  src/redis_client.cpp
  src/account_sessions.cpp
  src/characters.cpp)
target_include_directories(waw_persistence PUBLIC include)
target_link_libraries(waw_persistence PUBLIC waw_core PRIVATE asio::asio nlohmann_json::nlohmann_json)
waw_set_warnings(waw_persistence)
