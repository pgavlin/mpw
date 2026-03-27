add_library(M68K_LIB m68k_uc.cpp)
target_include_directories(M68K_LIB PUBLIC ${CMAKE_SOURCE_DIR}
                                           ${UNICORN_INCLUDE_DIRS}
                                           ${CAPSTONE_INCLUDE_DIRS})
target_link_directories(M68K_LIB PUBLIC ${UNICORN_LIBRARY_DIRS}
                                        ${CAPSTONE_LIBRARY_DIRS})
target_link_libraries(M68K_LIB PUBLIC ${UNICORN_LIBRARIES}
                                      ${CAPSTONE_LIBRARIES})
