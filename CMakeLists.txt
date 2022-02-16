cmake_minimum_required(VERSION 3.0.0)

project(metafs 
    VERSION 0.1.0
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

MESSAGE(STATUS "Project: metafs")

set(CMAKE_CXX_FLAGS "-std=c++11 -DERPC_INFINIBAND=true -D_FILE_OFFSET_BITS=64 -fpic -O2")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/bin)

include_directories(
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/thirdparty/
    ${PROJECT_SOURCE_DIR}/thirdparty/eRPC
    ${PROJECT_SOURCE_DIR}/thirdparty/eRPC/src
    ${PROJECT_SOURCE_DIR}/thirdparty/xxHash
)

set(COMMON_HEAD_LIST 
    ${PROJECT_SOURCE_DIR}/include/common
    ${PROJECT_SOURCE_DIR}/include/util
    ${PROJECT_SOURCE_DIR}/include/rpc
    ${PROJECT_SOURCE_DIR}/include/zk
    ${PROJECT_SOURCE_DIR}/thirdparty/eRPC/src
    ${PROJECT_SOURCE_DIR}/thirdparty/xxHash
    /usr/local/include/zookeeper
)

set(CLIENT_HEAD_LIST 
    ${COMMON_HEAD_LIST} 
    ${PROJECT_SOURCE_DIR}/include/client
)

set(SERVER_HEAD_LIST 
    ${COMMON_HEAD_LIST} 
    ${PROJECT_SOURCE_DIR}/include/server
)

file(GLOB CLIENT_SRC
    ${PROJECT_SOURCE_DIR}/src/common/*.cc
    ${PROJECT_SOURCE_DIR}/src/util/*.cc
    ${PROJECT_SOURCE_DIR}/src/client/*.cc
    ${PROJECT_SOURCE_DIR}/src/rpc/*.cc
    )

file(GLOB SERVER_SRC 
    ${PROJECT_SOURCE_DIR}/src/common/*.cc
    ${PROJECT_SOURCE_DIR}/src/util/*.cc
    ${PROJECT_SOURCE_DIR}/src/server/*.cc
    )

file(GLOB CLIENT_HEAD
    ${PROJECT_SOURCE_DIR}/include/common/*.h
    ${PROJECT_SOURCE_DIR}/include/util/*.h
    ${PROJECT_SOURCE_DIR}/include/client/*.h
    ${PROJECT_SOURCE_DIR}/include/rpc/*.h
    )

file(GLOB SERVER_HEAD
    ${PROJECT_SOURCE_DIR}/include/common/*.h
    ${PROJECT_SOURCE_DIR}/include/util/*.h
    ${PROJECT_SOURCE_DIR}/include/server/*.h
    ${PROJECT_SOURCE_DIR}/include/rpc/*.h
    )

link_libraries(
    ${PROJECT_SOURCE_DIR}/thirdparty/eRPC/build/liberpc.a
    ${PROJECT_SOURCE_DIR}/thirdparty/xxHash/libxxhash.a
)

add_library(metafs_client SHARED ${CLIENT_SRC})
add_executable(metafs_server  ${SERVER_SRC})

target_link_libraries(metafs_client
    rocksdb pthread numa dl ibverbs glog syscall_intercept memcached 
)

target_link_libraries(metafs_server
    pthread numa dl ibverbs glog snappy memcached rocksdb 
)

target_include_directories(metafs_client PUBLIC ${CLIENT_HEAD_LIST})
target_include_directories(metafs_server PUBLIC ${SERVER_HEAD_LIST})