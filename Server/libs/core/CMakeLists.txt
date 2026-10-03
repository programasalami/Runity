add_library(waw_core STATIC src/log.cpp src/tick_scheduler.cpp src/sha256.cpp src/task_queue.cpp)
target_include_directories(waw_core PUBLIC include)
waw_set_warnings(waw_core)
