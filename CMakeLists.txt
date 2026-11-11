cmake_minimum_required(VERSION 3.10)
project(ShmFrameTagger CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(OpenCV REQUIRED COMPONENTS core imgproc highgui)
find_package(Threads REQUIRED)

add_executable(ShmFrameTagger main.cpp)
target_include_directories(ShmFrameTagger PRIVATE ${OpenCV_INCLUDE_DIRS})
target_link_libraries(ShmFrameTagger PRIVATE ${OpenCV_LIBS} Threads::Threads)
