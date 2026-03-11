
add_library(RSRC_LIB rsrc.cpp rsrc_fork_accessor.cpp)
target_include_directories(RSRC_LIB PUBLIC "${CMAKE_SOURCE_DIR}")
