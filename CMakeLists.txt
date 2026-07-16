cmake_minimum_required(VERSION 3.16)

project(kvstore LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

add_compile_options($<$<CONFIG:Debug>:-g> $<$<CONFIG:Debug>:-O0>)

set(NETWORK "REACTOR" CACHE STRING "Network backend to build: REACTOR, PROACTOR, or COROUTINE")
set_property(CACHE NETWORK PROPERTY STRINGS REACTOR PROACTOR COROUTINE)

set(ENGINE "RBTREE_ENGINE" CACHE STRING "Storage engine to build: RBTREE_ENGINE, ARRAY_ENGINE HASH_ENGINE")
set_property(CACHE ENGINE PROPERTY STRINGS RBTREE_ENGINE ARRAY_ENGINE HASH_ENGINE SKIPLIST_ENGINE)

set(KVSTORE_PORT_NUM "20" CACHE STRING "Number of consecutive ports to listen on")
option(KVSTORE_ENABLE_TIMER "Enable connection timing logs" OFF)
option(ENABLE_MEMORY_POOL "Enable custom memory pool allocator" OFF)
option(ENABLE_TCMALLOC "Link with tcmalloc" OFF)
option(ENABLE_JEMALLOC "Link with jemalloc" OFF)
option(ENABLE_CPP_EXCEPTIONS "Enable C++ exceptions" OFF)

string(TOUPPER "${NETWORK}" NETWORK)
string(TOUPPER "${ENGINE}" ENGINE)

set(KVSTORE_NETWORK_OPTIONS REACTOR PROACTOR COROUTINE)
set(KVSTORE_ENGINE_OPTIONS RBTREE_ENGINE ARRAY_ENGINE HASH_ENGINE SKIPLIST_ENGINE)

if(NOT NETWORK IN_LIST KVSTORE_NETWORK_OPTIONS)
    message(FATAL_ERROR "NETWORK must be one of: ${KVSTORE_NETWORK_OPTIONS}")
endif()

if(NOT ENGINE IN_LIST KVSTORE_ENGINE_OPTIONS)
    message(FATAL_ERROR "ENGINE must be one of: ${KVSTORE_ENGINE_OPTIONS}")
endif()

find_package(Threads REQUIRED)

if(ENABLE_TCMALLOC AND ENABLE_JEMALLOC)
    message(FATAL_ERROR "ENABLE_TCMALLOC and ENABLE_JEMALLOC cannot both be ON")
endif()

if(ENABLE_TCMALLOC)
    find_library(TCMALLOC_LIBRARY NAMES tcmalloc_minimal tcmalloc REQUIRED)
endif()

if(ENABLE_JEMALLOC)
    find_library(JEMALLOC_LIBRARY NAMES jemalloc REQUIRED)
endif()

if(NOT ENABLE_CPP_EXCEPTIONS)
    add_compile_options($<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-fno-exceptions>)
endif()

add_library(kvstore_core STATIC
    memory/allocator.cpp
    memory/big_memory_allocator.cpp
    memory/central_pool.cpp
    memory/page_allocator.cpp
    memory/thread_cache.cpp
    network/network_utils.cpp
    core_engine/rbtree_engine/rbtree_engine.cpp
    core_engine/array_engine/array_engine.cpp
    core_engine/hash_engine/hash_engine.cpp
    core_engine/skiplist_engine/skiplist_engine.cpp
    persistent_core/kv_persistent.cpp
    timer/timer.cpp
    replication/rep_manager.cpp
)

target_include_directories(kvstore_core
    PUBLIC
        ${PROJECT_SOURCE_DIR}/core_engine
        ${PROJECT_SOURCE_DIR}/memory
        ${PROJECT_SOURCE_DIR}/network
        ${PROJECT_SOURCE_DIR}/protocal
        ${PROJECT_SOURCE_DIR}/persistent_core
        ${PROJECT_SOURCE_DIR}/base_component/rbtree
        ${PROJECT_SOURCE_DIR}/base_component/array
        ${PROJECT_SOURCE_DIR}/base_component/hash
        ${PROJECT_SOURCE_DIR}/base_component/skiplist
        ${PROJECT_SOURCE_DIR}/timer
        ${PROJECT_SOURCE_DIR}/replication
)

target_compile_definitions(kvstore_core
    PUBLIC
    ${ENGINE}
        PORT_NUM=${KVSTORE_PORT_NUM}
)

if(ENABLE_MEMORY_POOL)
    target_compile_definitions(kvstore_core PUBLIC ENABLE_MEMORY_POOL)
endif()

# The persistence layer uses io_uring for durable writes, so kvstore_core always needs liburing.
find_path(LIBURING_INCLUDE_DIR liburing.h REQUIRED)
find_library(LIBURING_LIBRARY uring REQUIRED)
target_include_directories(kvstore_core PUBLIC ${LIBURING_INCLUDE_DIR})
target_link_libraries(kvstore_core PUBLIC ${LIBURING_LIBRARY})

add_library(kvstore_backend STATIC)

target_link_libraries(kvstore_backend
    PUBLIC
        kvstore_core
        Threads::Threads
)

if(ENABLE_TCMALLOC)
    target_link_libraries(kvstore_backend PUBLIC ${TCMALLOC_LIBRARY})
endif()

if(ENABLE_JEMALLOC)
    target_link_libraries(kvstore_backend PUBLIC ${JEMALLOC_LIBRARY})
endif()

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
elseif(NETWORK STREQUAL "COROUTINE")
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
    target_compile_definitions(kvstore_backend PUBLIC COROUTINE)
    target_link_libraries(kvstore_backend PUBLIC ${CMAKE_DL_LIBS})
else()
    message(FATAL_ERROR "Unsupported NETWORK='${NETWORK}'. Use REACTOR, PROACTOR, or COROUTINE.")
endif()

add_executable(kvstore main.cpp)
target_link_libraries(kvstore PRIVATE kvstore_backend)

add_executable(kvstore_client
    kvstore_client/main.cpp
    kvstore_client/client.cpp
)
target_link_libraries(kvstore_client PRIVATE kvstore_core)

add_executable(kvstore_client_testcase
    kvstore_client/testcase.cpp
    kvstore_client/client.cpp
)
target_link_libraries(kvstore_client_testcase PRIVATE kvstore_core)
