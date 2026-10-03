find_package(asio CONFIG REQUIRED)

add_library(waw_net STATIC src/net_server.cpp)
target_include_directories(waw_net PUBLIC include)
target_link_libraries(waw_net PUBLIC waw_core waw_protocol PRIVATE asio::asio)
waw_set_warnings(waw_net)
