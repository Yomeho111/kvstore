cmake_minimum_required(VERSION 3.16)

project(kvstore LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(NETWORK "REACTOR" CACHE STRING "Network backend to build: REACTOR, PROACTOR, or CORROUTINE")
set_property(CACHE NETWORK PROPERTY STRINGS REACTOR PROACTOR CORROUTINE)

set(ENGINE "RBTREE_ENGINE" CACHE STRING "Storage engine to build: RBTREE_ENGINE")
set_property(CACHE ENGINE PROPERTY STRINGS RBTREE_ENGINE)

set(KVSTORE_PORT_NUM "20" CACHE STRING "Number of consecutive ports to listen on")
option(KVSTORE_ENABLE_TIMER "Enable connection timing logs" OFF)

string(TOUPPER "${NETWORK}" NETWORK)
string(TOUPPER "${ENGINE}" ENGINE)

set(KVSTORE_NETWORK_OPTIONS REACTOR PROACTOR CORROUTINE)
set(KVSTORE_ENGINE_OPTIONS RBTREE_ENGINE)

if(NOT NETWORK IN_LIST KVSTORE_NETWORK_OPTIONS)
    message(FATAL_ERROR "NETWORK must be one of: ${KVSTORE_NETWORK_OPTIONS}")
endif()

if(NOT ENGINE IN_LIST KVSTORE_ENGINE_OPTIONS)
    message(FATAL_ERROR "ENGINE must be one of: ${KVSTORE_ENGINE_OPTIONS}")
endif()

find_package(Threads REQUIRED)

add_library(kvstore_core STATIC
    memory/allocator.cpp
    core_engine/rbtree_engine/rbtree_engine.cpp
)

target_include_directories(kvstore_core
    PUBLIC
        ${PROJECT_SOURCE_DIR}/core_engine
        ${PROJECT_SOURCE_DIR}/memory
        ${PROJECT_SOURCE_DIR}/network
        ${PROJECT_SOURCE_DIR}/protocal
)

target_compile_definitions(kvstore_core
    PUBLIC
    ${ENGINE}
        PORT_NUM=${KVSTORE_PORT_NUM}
)

add_library(kvstore_backend STATIC)

target_link_libraries(kvstore_backend
    PUBLIC
        kvstore_core
        Threads::Threads
)

if(KVSTORE_ENABLE_TIMER)
    target_compile_definitions(kvstore_backend PUBLIC TIMER)
endif()

if(NETWORK STREQUAL "REACTOR")
    target_sources(kvstore_backend PRIVATE
        network/reactor/reactor.cpp
    )
    target_include_directories(kvstore_backend PUBLIC
        ${PROJECT_SOURCE_DIR}/network/reactor
    )
    target_compile_definitions(kvstore_backend PUBLIC REACTOR)
elseif(NETWORK STREQUAL "PROACTOR")
    find_path(LIBURING_INCLUDE_DIR liburing.h REQUIRED)
    find_library(LIBURING_LIBRARY uring REQUIRED)

    target_sources(kvstore_backend PRIVATE
        network/proactor/uring_tcp.cpp
    )
    target_include_directories(kvstore_backend PUBLIC
        ${PROJECT_SOURCE_DIR}/network/proactor
        ${LIBURING_INCLUDE_DIR}
    )
    target_compile_definitions(kvstore_backend PUBLIC PROACTOR)
    target_link_libraries(kvstore_backend PUBLIC ${LIBURING_LIBRARY})
elseif(NETWORK STREQUAL "CORROUTINE")
    target_sources(kvstore_backend PRIVATE
        network/my_coroutine/coroutine_server.cpp
        network/my_coroutine/my_coroutine/hpc_coroutine.cpp
        network/my_coroutine/my_coroutine/hpc_socket.cpp
        network/my_coroutine/my_coroutine/utils.cpp
    )
    target_include_directories(kvstore_backend PUBLIC
        ${PROJECT_SOURCE_DIR}/network/my_coroutine
        ${PROJECT_SOURCE_DIR}/network/my_coroutine/my_coroutine
    )
    target_compile_definitions(kvstore_backend PUBLIC CORROUTINE)
    target_link_libraries(kvstore_backend PUBLIC ${CMAKE_DL_LIBS})
else()
    message(FATAL_ERROR "Unsupported NETWORK='${NETWORK}'. Use REACTOR, PROACTOR, or CORROUTINE.")
endif()

add_executable(kvstore main.cpp)
target_link_libraries(kvstore PRIVATE kvstore_backend)

add_executable(kvstore_client_testcase
    kvstore_client/testcase.cpp
    kvstore_client/client.cpp
)
