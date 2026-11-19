# arm64 clipsyncd. Protocol sources are shared with the host tests.
add_executable(clipsyncd
    src/main.cpp
    src/daemon.cpp
    src/bridge.cpp
    src/net.cpp
    src/log.cpp
    src/frame.cpp
    src/fnv.cpp
    src/psk.cpp
    src/discovery.cpp
    src/discovery_net.cpp
    src/peer_store.cpp
    src/tcp_protocol.cpp
    src/tcp_sync.cpp
)
target_include_directories(clipsyncd PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(clipsyncd PRIVATE cxx_std_20)
target_compile_options(clipsyncd PRIVATE -Wall -Wextra -Werror -ffunction-sections -fdata-sections)
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(clipsyncd PRIVATE -O2)
endif()
target_link_options(clipsyncd PRIVATE -Wl,--gc-sections)
set_target_properties(clipsyncd PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(clipsyncd PRIVATE CLIPSYNC_DEBUG)
endif()
if(CMAKE_BUILD_TYPE STREQUAL "Release" AND CMAKE_STRIP)
    add_custom_command(TARGET clipsyncd POST_BUILD COMMAND ${CMAKE_STRIP} --strip-unneeded $<TARGET_FILE:clipsyncd>)
endif()
