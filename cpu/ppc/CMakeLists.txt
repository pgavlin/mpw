find_package(PkgConfig REQUIRED)
pkg_check_modules(UNICORN REQUIRED unicorn)
# CAPSTONE is found in top-level CMakeLists.txt

add_library(PPC_LIB ppc.cpp)
target_include_directories(PPC_LIB PUBLIC ${CMAKE_SOURCE_DIR} ${UNICORN_INCLUDE_DIRS} ${CAPSTONE_INCLUDE_DIRS})
target_link_directories(PPC_LIB PUBLIC ${UNICORN_LIBRARY_DIRS} ${CAPSTONE_LIBRARY_DIRS})
target_link_libraries(PPC_LIB PUBLIC ${UNICORN_LIBRARIES} ${CAPSTONE_LIBRARIES})

add_executable(ppc_test ppc_test.cpp)
target_link_libraries(ppc_test PPC_LIB M68K_LIB)
