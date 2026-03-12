
add_executable(mpw-lsp
    main.cpp
    transport.cpp
    jsonrpc.cpp
    server.cpp
    document_store.cpp
    tool_runner.cpp
    diagnostic_parser.cpp
)

target_include_directories(mpw-lsp PRIVATE "${CMAKE_SOURCE_DIR}/third_party")

install(
  PROGRAMS
    ${CMAKE_CURRENT_BINARY_DIR}/mpw-lsp
  DESTINATION bin
)
