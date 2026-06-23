cmake_minimum_required(VERSION 3.15)
project(ecproject)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--rpath=/home/hadoop/zzy/local/glibc-2.18/lib -Wl,--dynamic-linker=/home/hadoop/zzy/local/glibc-2.18/lib/ld-2.18.so")
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--rpath=/home/hadoop/zzy/local/glibc-2.18/lib -Wl,--dynamic-linker=/home/hadoop/zzy/local/glibc-2.18/lib/ld-2.18.so")

set(IF_REDIS 0)
set(IF_MEMCHACHED 0)

find_package(Threads REQUIRED)

# set the search path of include files
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include/ec
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/asio/asio/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/async_simple/async_simple
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/async_simple/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/yalantinglibs/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/yalantinglibs/include/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/gf-complete/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/jerasure/include/jerasure
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/jerasure/include
)

if(IF_MEMCHACHED EQUAL 1)
    include_directories(
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libmemcached/include
    )

    link_directories(
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libmemcached/lib
    )
endif()

if(IF_REDIS EQUAL 1)
    include_directories(
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/hiredis/include
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/redis-plus-plus/include
    )

    link_directories(
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/hiredis/lib
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/redis-plus-plus/lib
    )
endif()

# set the search path of lib files
link_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/jerasure/lib
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/gf-complete/lib
)

# set the shared source files
set(SHARED_SOURCES
    src/ec/utils.cpp
    src/ec/erasure_code.cpp
    src/ec/rs.cpp
    src/ec/lrc.cpp
    src/ec/pc.cpp
)

add_executable(test_rs
    src/tests/test_rs.cpp
    ${SHARED_SOURCES}
)

add_executable(test_lrc
    src/tests/test_lrc.cpp
    ${SHARED_SOURCES}
)

add_executable(test_pc
    src/tests/test_pc.cpp
    ${SHARED_SOURCES}
)

add_executable(run_coordinator
    src/metadata.cpp
    src/tinyxml2.cpp
    src/coordinator/auxs.cpp
    src/coordinator/placement.cpp
    src/coordinator/repair.cpp
    src/coordinator/merge.cpp
    src/coordinator/coordinator.cpp
    src/coordinator/run_coordinator.cpp
    ${SHARED_SOURCES}
)

add_executable(run_proxy
    src/metadata.cpp
    src/tinyxml2.cpp
    src/proxy/proxy.cpp
    src/proxy/handle_repair.cpp
    src/proxy/handle_merge.cpp
    src/proxy/run_proxy.cpp
    ${SHARED_SOURCES}
)

add_executable(run_datanode
    src/metadata.cpp    
    src/datanode/datanode.cpp
    src/datanode/run_datanode.cpp
    ${SHARED_SOURCES}
)

add_executable(run_client
    src/metadata.cpp
    src/client/client.cpp
    src/client/run_client.cpp
    ${SHARED_SOURCES}
)

target_link_libraries(test_rs Jerasure gf_complete)
target_link_libraries(test_lrc Jerasure gf_complete)
target_link_libraries(test_pc Jerasure gf_complete)

if(IF_MEMCHACHED EQUAL 1)
    target_link_libraries(run_coordinator Threads::Threads Jerasure gf_complete memcached)
    target_link_libraries(run_proxy Threads::Threads Jerasure gf_complete memcached)
    target_link_libraries(run_datanode Threads::Threads Jerasure gf_complete memcached)
    target_link_libraries(run_client Threads::Threads Jerasure gf_complete memcached)
elseif(IF_REDIS EQUAL 1)
    target_link_libraries(run_coordinator Threads::Threads Jerasure gf_complete redis++)
    target_link_libraries(run_proxy Threads::Threads Jerasure gf_complete redis++)
    target_link_libraries(run_datanode Threads::Threads Jerasure gf_complete redis++)
    target_link_libraries(run_client Threads::Threads Jerasure gf_complete redis++)
else()
    target_link_libraries(run_coordinator Threads::Threads Jerasure gf_complete)
    target_link_libraries(run_proxy Threads::Threads Jerasure gf_complete)
    target_link_libraries(run_datanode Threads::Threads Jerasure gf_complete)
    target_link_libraries(run_client Threads::Threads Jerasure gf_complete)
endif()
