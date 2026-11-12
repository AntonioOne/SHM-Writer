cmake_minimum_required(VERSION 3.10)
project(ShmFrameTagger CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Link libstdc++/libgcc statically so the binary only needs glibc on the target machine.
option(STATIC_RUNTIME "Link libstdc++ and libgcc statically" ON)

add_executable(ShmFrameTagger
    src/main.cpp
    src/segment.cpp
    src/digits.cpp)
target_compile_options(ShmFrameTagger PRIVATE -Wall -Wextra)
if(STATIC_RUNTIME)
    target_link_libraries(ShmFrameTagger PRIVATE -static-libstdc++ -static-libgcc)
endif()
