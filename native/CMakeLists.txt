cmake_minimum_required(VERSION 3.16)
project(container_edge_runtime VERSION 3.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -march=x86-64-v3 -mtune=generic")
set(CMAKE_C_FLAGS_RELEASE "-O3 -DNDEBUG -march=x86-64-v3 -mtune=generic")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -Wall -Wextra")

include(FetchContent)
FetchContent_Declare(
    randomx
    GIT_REPOSITORY https://github.com/tevador/RandomX.git
    GIT_TAG aaafe71322df6602c21a5c72937ac284724ae561 # v2.0.1
)
FetchContent_MakeAvailable(randomx)

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(JSONCPP REQUIRED jsoncpp)

set(ENGINE_SOURCES
    src/main.cpp
    src/http_server.cpp
    src/engine.cpp
    src/pool_client.cpp
    src/utils/config.cpp
)

add_executable(edge-runtime ${ENGINE_SOURCES})
target_include_directories(edge-runtime PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${JSONCPP_INCLUDE_DIRS}
    ${randomx_SOURCE_DIR}/src
)
target_compile_options(edge-runtime PRIVATE ${JSONCPP_CFLAGS_OTHER})
target_link_libraries(edge-runtime PRIVATE
    randomx
    Threads::Threads
    OpenSSL::SSL
    OpenSSL::Crypto
    ${JSONCPP_LIBRARIES}
)
set_target_properties(edge-runtime PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_custom_command(TARGET edge-runtime POST_BUILD
        COMMAND ${CMAKE_STRIP} $<TARGET_FILE:edge-runtime>
    )
endif()
