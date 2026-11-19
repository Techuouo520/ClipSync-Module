# Top-level build. Host configures tests. Android NDK configures clipsyncd.
cmake_minimum_required(VERSION 3.21)
project(clipsync LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(ANDROID)
    add_subdirectory(native)
else()
    add_subdirectory(test)
endif()
