find_package(pugixml CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(ZLIB REQUIRED)

add_library(runity_content STATIC src/content_db.cpp src/behavior_loader.cpp)
target_include_directories(runity_content PUBLIC include)
target_link_libraries(runity_content PUBLIC runity_core PRIVATE pugixml::pugixml nlohmann_json::nlohmann_json ZLIB::ZLIB)
runity_set_warnings(runity_content)
