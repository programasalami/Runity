add_library(runity_core STATIC src/log.cpp src/tick_scheduler.cpp src/sha256.cpp src/task_queue.cpp)
target_include_directories(runity_core PUBLIC include)
runity_set_warnings(runity_core)
