cmake_minimum_required(VERSION 3.28)

project(websocket_service)

find_package(CURL REQUIRED)
find_package(libwebsockets CONFIG  REQUIRED)

add_executable(websocket_service)

aux_source_directory(src source_dir)

target_sources(websocket_service PRIVATE ${source_dir})

target_include_directories(websocket_service PRIVATE
    include
)

set_target_properties(websocket_service PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin"
)

# 启用常用编译警告（不开启 -Werror，避免历史告警阻塞构建）
target_compile_options(websocket_service PRIVATE -Wall -Wextra)

# 使用变量链接库
target_link_libraries(websocket_service
    websockets
    CURL::libcurl
)