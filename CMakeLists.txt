cmake_minimum_required(VERSION 3.16)
project(bitjams CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(ftxui
    GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
    GIT_TAG        v6.1.9
)
FetchContent_MakeAvailable(ftxui)

add_executable(bitjams
    src/main.cpp
    src/audio.cpp
    src/session.cpp
    src/ui.cpp
    src/network.cpp
)

target_include_directories(bitjams PRIVATE include)

target_link_libraries(bitjams PRIVATE
    ftxui::component  # transitively pulls in ftxui::dom and ftxui::screen
)

if(APPLE)
    target_link_libraries(bitjams PRIVATE
        "-framework CoreAudio"
        "-framework AudioToolbox"
        "-framework CoreFoundation"
    )
elseif(WIN32)
    target_link_libraries(bitjams PRIVATE ole32 winmm ws2_32)
elseif(UNIX)
    target_link_libraries(bitjams PRIVATE pthread dl m)
endif()

if(MSVC)
    target_compile_options(bitjams PRIVATE /W3 /O2)
else()
    target_compile_options(bitjams PRIVATE -Wall -Wextra -O2)
endif()

# --- Tests -----------------------------------------------------------------
enable_testing()

add_executable(test_net
    tests/test_net.cpp
    src/session.cpp
    src/network.cpp
)
target_include_directories(test_net PRIVATE include src)
if(APPLE)
    # No platform libs required for loopback TCP on macOS.
elseif(WIN32)
    target_link_libraries(test_net PRIVATE ws2_32)
elseif(UNIX)
    target_link_libraries(test_net PRIVATE pthread)
endif()
if(MSVC)
    target_compile_options(test_net PRIVATE /W3 /O2)
else()
    target_compile_options(test_net PRIVATE -Wall -Wextra -O2)
endif()
add_test(NAME net_tests COMMAND test_net)

add_executable(test_record
    tests/test_record.cpp
    src/session.cpp
)
target_include_directories(test_record PRIVATE include src)
if(APPLE)
elseif(UNIX)
    target_link_libraries(test_record PRIVATE pthread)
endif()
if(MSVC)
    target_compile_options(test_record PRIVATE /W3 /O2)
else()
    target_compile_options(test_record PRIVATE -Wall -Wextra -O2)
endif()

add_test(NAME record_tests COMMAND test_record)
