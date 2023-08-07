cmake_minimum_required(VERSION 3.25)
project(ThreadPool CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(folly REQUIRED)
include_directories("/opt/homebrew/Cellar/folly/2023.07.24.00/include")
link_directories("/opt/homebrew/Cellar/folly/2023.07.24.00/lib")

find_package(boost REQUIRED)
include_directories("/opt/homebrew/Cellar/boost/1.82.0_1/include")
link_directories("/opt/homebrew/Cellar/boost/1.82.0_1/lib")

find_package(glog REQUIRED)
include_directories("/opt/homebrew/Cellar/glog/0.6.0/include")
link_directories("/opt/homebrew/Cellar/glog/0.6.0/lib")

find_package(gflags REQUIRED)
include_directories("/opt/homebrew/Cellar/gflags/2.2.2/include")
link_directories("/opt/homebrew/Cellar/gflags/2.2.2/lib")

find_package(fmt REQUIRED)
include_directories("/opt/homebrew/Cellar/fmt/10.0.0/include")
link_directories("/opt/homebrew/Cellar/fmt/10.0.0/lib")

include_directories(ThreadPool)

file(GLOB SRC ThreadPool/*.cpp)
file(GLOB HEADERS ThreadPool/*.h)

set(CMAKE_CXX_FLAGS -lglog)

#add_executable(ThreadPool ${SRC} main.cpp)
add_executable(ThreadPool ${SRC} ${HEADERS} main.cpp)