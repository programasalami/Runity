add_library(waw_protocol STATIC src/generated/messages.cpp)
target_include_directories(waw_protocol PUBLIC include)
target_link_libraries(waw_protocol PUBLIC waw_core)
waw_set_warnings(waw_protocol)
