find_package(pugixml CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(ZLIB REQUIRED)

add_library(waw_content STATIC src/content_db.cpp src/behavior_loader.cpp)
target_include_directories(waw_content PUBLIC include)
target_link_libraries(waw_content PUBLIC waw_core PRIVATE pugixml::pugixml nlohmann_json::nlohmann_json ZLIB::ZLIB)
waw_set_warnings(waw_content)
