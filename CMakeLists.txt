cmake_minimum_required(VERSION 3.16)

project(kvstore LANGUAGES C CXX)

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

# ---------------------------------------------------------------------------
# eBPF replication probe
#
# Mirrors third_party/libbpf-bootstrap/examples/c/Makefile: build the vendored
# static libbpf and the bootstrap bpftool, compile delta.bpf.c for the BPF
# target, then generate the delta.skel.h that replicate.cpp includes.
# ---------------------------------------------------------------------------

set(BPF_BOOTSTRAP_DIR ${PROJECT_SOURCE_DIR}/third_party/libbpf-bootstrap)
set(LIBBPF_SRC_DIR ${BPF_BOOTSTRAP_DIR}/libbpf/src)
set(BPFTOOL_SRC_DIR ${BPF_BOOTSTRAP_DIR}/bpftool/src)

set(BPF_OUTPUT_DIR ${CMAKE_BINARY_DIR}/bpf)
set(LIBBPF_STATIC ${BPF_OUTPUT_DIR}/libbpf.a)
set(BPFTOOL_OUTPUT_DIR ${BPF_OUTPUT_DIR}/bpftool)
set(BPFTOOL_EXE ${BPFTOOL_OUTPUT_DIR}/bootstrap/bpftool)

# Same normalisation the reference Makefile applies to `uname -m`.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64")
    set(BPF_ARCH x86)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
    set(BPF_ARCH arm64)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^arm")
    set(BPF_ARCH arm)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "ppc64le")
    set(BPF_ARCH powerpc)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "mips")
    set(BPF_ARCH mips)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "riscv64")
    set(BPF_ARCH riscv)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "loongarch64")
    set(BPF_ARCH loongarch)
else()
    set(BPF_ARCH ${CMAKE_SYSTEM_PROCESSOR})
endif()

set(BPF_VMLINUX_H ${BPF_BOOTSTRAP_DIR}/vmlinux.h/include/${BPF_ARCH}/vmlinux.h)
if(NOT EXISTS ${BPF_VMLINUX_H})
    message(FATAL_ERROR "No vendored vmlinux.h for arch '${BPF_ARCH}': ${BPF_VMLINUX_H}")
endif()

find_program(BPF_CLANG_EXE
    NAMES clang clang-20 clang-19 clang-18 clang-17 clang-16 clang-15
    DOC "clang used to compile BPF programs")

if(NOT BPF_CLANG_EXE)
    message(FATAL_ERROR
        "clang is required to compile replicate/delta.bpf.c (gcc cannot target BPF).\n"
        "Install the eBPF build prerequisites:\n"
        "  sudo apt-get install -y clang libelf-dev zlib1g-dev")
endif()

# Clang's own system include dirs, added with -idirafter so the BPF target still
# finds asm/types.h and friends. Same trick as CLANG_BPF_SYS_INCLUDES in the Makefile.
execute_process(
    COMMAND bash -c "${BPF_CLANG_EXE} -v -E - </dev/null 2>&1 | \
        sed -n '/<...> search starts here:/,/End of search list./{ s| \\(/.*\\)|-idirafter \\1|p }'"
    OUTPUT_VARIABLE BPF_CLANG_SYS_INCLUDES_RAW
    RESULT_VARIABLE BPF_CLANG_SYS_INCLUDES_RESULT
    OUTPUT_STRIP_TRAILING_WHITESPACE)

if(NOT BPF_CLANG_SYS_INCLUDES_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to query clang system include paths from ${BPF_CLANG_EXE}")
endif()

separate_arguments(BPF_CLANG_SYS_INCLUDES UNIX_COMMAND "${BPF_CLANG_SYS_INCLUDES_RAW}")

# libbpf installs headers to ${BPF_OUTPUT_DIR}/bpf and the archive to ${BPF_OUTPUT_DIR}.
add_custom_command(
    OUTPUT ${LIBBPF_STATIC}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BPF_OUTPUT_DIR}/libbpf
    COMMAND make -C ${LIBBPF_SRC_DIR}
        BUILD_STATIC_ONLY=1
        OBJDIR=${BPF_OUTPUT_DIR}/libbpf
        DESTDIR=${BPF_OUTPUT_DIR}
        INCLUDEDIR=
        LIBDIR=
        UAPIDIR=
        install
    DEPENDS ${LIBBPF_SRC_DIR}/Makefile
    COMMENT "[libbpf] Building static libbpf"
    VERBATIM)

add_custom_command(
    OUTPUT ${BPFTOOL_EXE}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BPFTOOL_OUTPUT_DIR}
    COMMAND make ARCH= CROSS_COMPILE= OUTPUT=${BPFTOOL_OUTPUT_DIR}/ -C ${BPFTOOL_SRC_DIR} bootstrap
    DEPENDS ${BPFTOOL_SRC_DIR}/Makefile
    COMMENT "[bpftool] Building bootstrap bpftool"
    VERBATIM)

set(DELTA_BPF_SRC ${PROJECT_SOURCE_DIR}/replicate/delta.bpf.c)
set(DELTA_BPF_OBJ ${BPF_OUTPUT_DIR}/delta.bpf.o)
set(DELTA_BPF_TMP_OBJ ${BPF_OUTPUT_DIR}/delta.tmp.bpf.o)
set(DELTA_SKEL_H ${BPF_OUTPUT_DIR}/delta.skel.h)

# `bpftool gen object` performs the final BPF linking step, as in the Makefile.
add_custom_command(
    OUTPUT ${DELTA_BPF_OBJ}
    COMMAND ${BPF_CLANG_EXE} -g -O2 -target bpf -D__TARGET_ARCH_${BPF_ARCH}
        -I${BPF_OUTPUT_DIR}
        -I${BPF_BOOTSTRAP_DIR}/libbpf/include/uapi
        -I${BPF_BOOTSTRAP_DIR}/vmlinux.h/include/${BPF_ARCH}
        -I${PROJECT_SOURCE_DIR}/replicate
        ${BPF_CLANG_SYS_INCLUDES}
        -c ${DELTA_BPF_SRC} -o ${DELTA_BPF_TMP_OBJ}
    COMMAND ${BPFTOOL_EXE} gen object ${DELTA_BPF_OBJ} ${DELTA_BPF_TMP_OBJ}
    DEPENDS ${DELTA_BPF_SRC} ${PROJECT_SOURCE_DIR}/replicate/delta.h
            ${BPF_VMLINUX_H} ${LIBBPF_STATIC} ${BPFTOOL_EXE}
    COMMAND_EXPAND_LISTS
    COMMENT "[BPF] Building ${DELTA_BPF_OBJ}"
    VERBATIM)

add_custom_command(
    OUTPUT ${DELTA_SKEL_H}
    COMMAND bash -c "${BPFTOOL_EXE} gen skeleton ${DELTA_BPF_OBJ} > ${DELTA_SKEL_H}"
    DEPENDS ${DELTA_BPF_OBJ} ${BPFTOOL_EXE}
    COMMENT "[GEN-SKEL] Building ${DELTA_SKEL_H}"
    VERBATIM)

add_custom_target(delta_skel DEPENDS ${DELTA_SKEL_H})

# The replication transport needs the RDMA verbs/CM libraries.
find_path(IBVERBS_INCLUDE_DIR infiniband/verbs.h REQUIRED)
find_library(IBVERBS_LIBRARY ibverbs REQUIRED)
find_library(RDMACM_LIBRARY rdmacm REQUIRED)
find_library(ELF_LIBRARY elf REQUIRED)
find_library(Z_LIBRARY z REQUIRED)

add_library(kvstore_core STATIC
    memory/allocator.cpp
    memory/big_memory_allocator.cpp
    memory/central_pool.cpp
    memory/page_allocator.cpp
    memory/thread_cache.cpp
    config/kv_config.cpp
    logger/kv_log.cpp
    network/network_utils.cpp
    core_engine/rbtree_engine/rbtree_engine.cpp
    core_engine/array_engine/array_engine.cpp
    core_engine/hash_engine/hash_engine.cpp
    core_engine/skiplist_engine/skiplist_engine.cpp
    persistent_core/kv_persistent.cpp
    replicate/replicate.cpp
    replicate/netbuffer.cpp
    timer/timer.cpp
    third_party/hiredis/alloc.c
    third_party/hiredis/async.c
    third_party/hiredis/hiredis.c
    third_party/hiredis/net.c
    third_party/hiredis/read.c
    third_party/hiredis/sds.c
    third_party/hiredis/sockcompat.c
)

target_include_directories(kvstore_core
    PUBLIC
        ${PROJECT_SOURCE_DIR}/core_engine
        ${PROJECT_SOURCE_DIR}/memory
        ${PROJECT_SOURCE_DIR}/network
        ${PROJECT_SOURCE_DIR}/protocal
        ${PROJECT_SOURCE_DIR}/persistent_core
        ${PROJECT_SOURCE_DIR}/config
        ${PROJECT_SOURCE_DIR}/logger
        ${PROJECT_SOURCE_DIR}/base_component/rbtree
        ${PROJECT_SOURCE_DIR}/base_component/array
        ${PROJECT_SOURCE_DIR}/base_component/hash
        ${PROJECT_SOURCE_DIR}/base_component/skiplist
        ${PROJECT_SOURCE_DIR}/timer
        ${PROJECT_SOURCE_DIR}/replicate
        ${PROJECT_SOURCE_DIR}/third_party/hiredis
        ${BPF_OUTPUT_DIR}
        ${BPF_BOOTSTRAP_DIR}/libbpf/include/uapi
)

# delta.skel.h is generated, so it must exist before any replicate TU compiles.
add_dependencies(kvstore_core delta_skel)

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

target_link_libraries(kvstore_core
    PUBLIC
        ${LIBBPF_STATIC}
        ${ELF_LIBRARY}
        ${Z_LIBRARY}
        ${IBVERBS_LIBRARY}
        ${RDMACM_LIBRARY}
)

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
