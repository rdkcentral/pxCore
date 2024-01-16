cmake_minimum_required(VERSION 2.8)
project(rtcore)
find_package(PkgConfig)

set(CMAKE_CXX_STANDARD 14)

option(RTCORE_REUSE_CURL_HANDLES "RTCORE_REUSE_CURL_HANDLES" ON)
option(RTCORE_RTVALUE_CAST_UINT_BASIC "RTCORE_RTVALUE_CAST_UINT_BASIC" ON)
option(RTCORE_COMPILE_WARNINGS_AS_ERRORS "RTCORE_COMPILE_WARNINGS_AS_ERRORS" ON)

set(EXTDIR "${CMAKE_CURRENT_SOURCE_DIR}/../external")

include(../cmake/CommDeps.cmake)

include_directories(BEFORE "${CMAKE_CURRENT_SOURCE_DIR}/../../include")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC -g")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-deprecated -fPIC -g")
add_definitions(${COMM_DEPS_DEFINITIONS})
include_directories(AFTER
      ${COMM_DEPS_INCLUDE_DIRS}
)

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../build/rtcore)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../build/rtcore)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../build/rtcore)
add_definitions(-DRT_PLATFORM_LINUX -DENABLE_HTTP_CACHE -DRUNINMAIN -DRAPIDJSON_HAS_STDSTRING)

if (DEFINED ENV{CODE_COVERAGE})
    message("enabling code coverage support")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fprofile-arcs -ftest-coverage -fno-inline")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fprofile-arcs -ftest-coverage -fno-inline")
    add_definitions(-DENABLE_CODE_COVERAGE=1)
endif ()

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")

if (RTCORE_COMPILE_WARNINGS_AS_ERRORS)
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Werror")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Werror")
endif (RTCORE_COMPILE_WARNINGS_AS_ERRORS)

include_directories(AFTER ${CMAKE_CURRENT_SOURCE_DIR})

if (RTCORE_REUSE_CURL_HANDLES)
    message("Enabling curl handle reuse")
    add_definitions(-DRTCORE_REUSE_DOWNLOAD_HANDLES)
endif (RTCORE_REUSE_CURL_HANDLES)

if (RTCORE_RTVALUE_CAST_UINT_BASIC)
    message("Enabling basic unsigned int casting")
    add_definitions(-DRTCORE_RTVALUE_CAST_UINT_BASIC)
endif (RTCORE_RTVALUE_CAST_UINT_BASIC)

if (RTCORE_ETAG_AVOID_NONSTALE)
    message("ETag avoided for non-stale objects")
    add_definitions(-DRTCORE_ETAG_AVOID_NONSTALE)
endif (RTCORE_ETAG_AVOID_NONSTALE)

set(RTCORE_FILES utf8.c rtString.cpp rtLog.cpp rtValue.cpp rtError.cpp ioapi_mem.c rtPromise.cpp rtObject.cpp)

set(RTCORE_PLATFORM_FILES "")

if (APPLE)
    set(RTCORE_PLATFORM_FILES "mac/pxTimerNative.mm")
else ()
    set(RTCORE_PLATFORM_FILES "gles/pxTimerNative.cpp")
endif (APPLE)

set(RTCORE_FILES ${RTCORE_FILES}  rtFileCache.cpp rtHttpRequest.cpp   rtHttpResponse.cpp  rtThreadQueue.cpp  unix/rtThreadPoolNative.cpp rtFileDownloader.cpp unix/rtMutexNative.cpp rtThreadTask.cpp rtHttpCache.cpp rtThreadPool.cpp rtUrlUtils.cpp rtFile.cpp rtSettings.cpp rtJsonUtils.cpp rtPathUtils.cpp rtCORS.cpp ${RTCORE_PLATFORM_FILES}
       )

include_directories(AFTER ${EXTDIR})

message("Building rtcore libs")
add_library(rtCore_s ${RTCORE_FILES})
add_library(rtCore SHARED ${RTCORE_FILES})
set(RTCORE_LINK_LIBRARIES "curl")
set(RTCORE_LINK_LIBRARIES_PATH ${CMAKE_CURRENT_SOURCE_DIR}/../../extlibs/lib)
target_link_libraries(rtCore -L${RTCORE_LINK_LIBRARIES_PATH} ${RTCORE_LINK_LIBRARIES})
