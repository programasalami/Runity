find_package(asio CONFIG REQUIRED)

add_library(runity_net STATIC src/net_server.cpp)
target_include_directories(runity_net PUBLIC include)
target_link_libraries(runity_net PUBLIC runity_core runity_protocol PRIVATE asio::asio)
runity_set_warnings(runity_net)
