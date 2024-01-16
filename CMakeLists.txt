cmake_minimum_required(VERSION 2.8)
project(rtCore)

option(ENABLE_THREAD_SANITIZER "ENABLE_THREAD_SANITIZER" OFF)
option(ENABLE_ADDRESS_SANITIZER "ENABLE_ADDRESS_SANITIZER" OFF)
option(PREFER_SYSTEM_LIBRARIES "Prefer to use system libraries over bundled." "OFF")

if (ENABLE_THREAD_SANITIZER)
    message("Enabling thread sanitizer")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=thread")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=thread")
endif (ENABLE_THREAD_SANITIZER)

if (ENABLE_ADDRESS_SANITIZER)
    message("Enabling address sanitizer")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=address")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address")
endif (ENABLE_ADDRESS_SANITIZER)

message("Building rtCore")
add_subdirectory(src)
