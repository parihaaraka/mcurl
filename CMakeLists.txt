cmake_minimum_required(VERSION 3.10)

project(mcurl C CXX)

set(CMAKE_DISABLE_IN_SOURCE_BUILD ON)
set(CMAKE_DISABLE_SOURCE_CHANGES  ON)

set(CMAKE_VERBOSE_MAKEFILE ON)
set(CMAKE_COLOR_MAKEFILE   ON)

set (CMAKE_CXX_STANDARD 11)

#set(CMAKE_POSITION_INDEPENDENT_CODE ON)

file(GLOB MCURL_FILES *.cpp)
file(GLOB B64_FILES b64/*.c*)
include_directories(BEFORE .)

add_library(b64 STATIC ${B64_FILES})
add_library(${PROJECT_NAME} STATIC ${MCURL_FILES})

add_definitions(-DCURL_STATICLIB)
find_package(CURL REQUIRED)
message(STATUS ">>> CURL Dir Found: " ${CURL_INCLUDE_DIR})

include_directories(${CURL_INCLUDE_DIRS})
link_directories(${CURL_LIBRARIES})

target_link_libraries(${PROJECT_NAME} LINK_PUBLIC b64 ev curl)
