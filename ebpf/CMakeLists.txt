# Standalone eBPF delta transfer daemon. It attaches to a running kvstore server
# from the outside, so it is built as its own executable and is never linked
# into kvstore or kvstore_core.

set(LIBBPF_BOOTSTRAP_DIR ${PROJECT_SOURCE_DIR}/third_party/libbpf-bootstrap)

if(NOT EXISTS ${LIBBPF_BOOTSTRAP_DIR}/libbpf/src/Makefile)
    message(FATAL_ERROR
        "third_party/libbpf-bootstrap is not populated. Run:\n"
        "  git submodule update --init --recursive")
endif()

list(APPEND CMAKE_MODULE_PATH ${LIBBPF_BOOTSTRAP_DIR}/tools/cmake)

include(ExternalProject)

set(EBPF_LIBBPF_DIR ${CMAKE_CURRENT_BINARY_DIR}/libbpf)
set(EBPF_BPFTOOL_DIR ${CMAKE_CURRENT_BINARY_DIR}/bpftool)

# The vendored libbpf and bpftool are plain Makefile projects.
ExternalProject_Add(ebpf_libbpf
    PREFIX libbpf-prefix
    SOURCE_DIR ${LIBBPF_BOOTSTRAP_DIR}/libbpf/src
    CONFIGURE_COMMAND ""
    BUILD_COMMAND make
        CC=${CMAKE_C_COMPILER}
        BUILD_STATIC_ONLY=1
        OBJDIR=${EBPF_LIBBPF_DIR}/libbpf
        DESTDIR=${EBPF_LIBBPF_DIR}
        INCLUDEDIR=
        LIBDIR=
        UAPIDIR=
        install install_uapi_headers
    BUILD_IN_SOURCE TRUE
    INSTALL_COMMAND ""
    STEP_TARGETS build
    BUILD_BYPRODUCTS ${EBPF_LIBBPF_DIR}/libbpf.a
)

# bpftool's Makefile refuses to create OUTPUT itself.
file(MAKE_DIRECTORY ${EBPF_BPFTOOL_DIR})

ExternalProject_Add(ebpf_bpftool
    PREFIX bpftool-prefix
    SOURCE_DIR ${LIBBPF_BOOTSTRAP_DIR}/bpftool/src
    CONFIGURE_COMMAND ""
    BUILD_COMMAND make ARCH= CROSS_COMPILE= OUTPUT=${EBPF_BPFTOOL_DIR}/ bootstrap
    BUILD_IN_SOURCE TRUE
    INSTALL_COMMAND ""
    STEP_TARGETS build
    BUILD_BYPRODUCTS ${EBPF_BPFTOOL_DIR}/bootstrap/bpftool
)

# vmlinux.h/include/ is named after the kernel arch, not the uname arch.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64")
    set(EBPF_VMLINUX_ARCH x86)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
    set(EBPF_VMLINUX_ARCH arm64)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "arm")
    set(EBPF_VMLINUX_ARCH arm)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "ppc64le")
    set(EBPF_VMLINUX_ARCH powerpc)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "riscv64")
    set(EBPF_VMLINUX_ARCH riscv)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "loongarch64")
    set(EBPF_VMLINUX_ARCH loongarch)
else()
    set(EBPF_VMLINUX_ARCH ${CMAKE_SYSTEM_PROCESSOR})
endif()

# Override with -DBPFOBJECT_VMLINUX_H=... to build against the running kernel's BTF.
set(BPFOBJECT_VMLINUX_H
    ${LIBBPF_BOOTSTRAP_DIR}/vmlinux.h/include/${EBPF_VMLINUX_ARCH}/vmlinux.h
    CACHE FILEPATH "vmlinux.h the BPF object is compiled against")

set(BPFOBJECT_BPFTOOL_EXE ${EBPF_BPFTOOL_DIR}/bootstrap/bpftool)
set(LIBBPF_INCLUDE_DIRS ${EBPF_LIBBPF_DIR})
set(LIBBPF_LIBRARIES ${EBPF_LIBBPF_DIR}/libbpf.a)

find_package(BpfObject REQUIRED)

# Produces delta_transfer.bpf.o and the delta_transfer_skel interface library
# carrying delta_transfer.skel.h, the libbpf headers and -lelf -lz.
bpf_object(delta_transfer delta_transfer.bpf.c delta.h)
add_dependencies(delta_transfer_skel ebpf_libbpf ebpf_bpftool)

add_executable(delta_transfer
    delta_transfer.cpp
    cache_pool.cpp
    tcp_server.cpp
)

target_include_directories(delta_transfer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(delta_transfer PRIVATE delta_transfer_skel)
