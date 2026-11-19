# Host tests for frame, FNV-1a, PSK, echo suppression, and UDP packets.
add_executable(clipsync_tests
    test_main.cpp
    ${CMAKE_SOURCE_DIR}/native/src/frame.cpp
    ${CMAKE_SOURCE_DIR}/native/src/fnv.cpp
    ${CMAKE_SOURCE_DIR}/native/src/psk.cpp
    ${CMAKE_SOURCE_DIR}/native/src/packet.cpp
    ${CMAKE_SOURCE_DIR}/native/src/discovery.cpp
    ${CMAKE_SOURCE_DIR}/native/src/tcp_protocol.cpp
)
target_include_directories(clipsync_tests PRIVATE ${CMAKE_SOURCE_DIR}/native/include)
target_compile_features(clipsync_tests PRIVATE cxx_std_20)
set_target_properties(clipsync_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
if(MSVC)
    target_compile_options(clipsync_tests PRIVATE /W4 /permissive-)
else()
    target_compile_options(clipsync_tests PRIVATE -Wall -Wextra -Werror)
endif()
