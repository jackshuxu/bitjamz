cmake_minimum_required(VERSION 3.16)
project(bitjams CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(bitjams src/main.cpp)

target_include_directories(bitjams PRIVATE include)

if(APPLE)
    target_link_libraries(bitjams
        "-framework CoreAudio"
        "-framework AudioToolbox"
        "-framework CoreFoundation"
    )
elseif(UNIX)
    target_link_libraries(bitjams pthread dl m)
endif()

target_compile_options(bitjams PRIVATE -O2)
