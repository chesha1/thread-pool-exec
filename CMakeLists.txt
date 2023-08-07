cmake_minimum_required(VERSION 3.25)
project(ThreadPool CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(folly REQUIRED)
include_directories("/opt/homebrew/Cellar/folly/2023.07.24.00/include")
link_directories("/opt/homebrew/Cellar/folly/2023.07.24.00/lib")

find_package(Boost REQUIRED)
include_directories("/opt/homebrew/Cellar/boost/1.82.0_1/include")
link_directories("/opt/homebrew/Cellar/boost/1.82.0_1/lib")

include_directories(ThreadPool)

file(GLOB SRC ThreadPool/*.cpp)
file(GLOB HEADERS ThreadPool/*.h)

add_executable(ThreadPool ${SRC} ${HEADERS} main.cpp)