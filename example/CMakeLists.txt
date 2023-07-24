cmake_minimum_required(VERSION 2.8)

project(mcurl_example)
set(CMAKE_CXX_STANDARD 17)
add_executable(${PROJECT_NAME} "main.cpp")
add_subdirectory(.. ./mcurl)
include_directories(..)

target_link_libraries(${PROJECT_NAME} mcurl)
