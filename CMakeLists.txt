cmake_minimum_required(VERSION 3.16)
project(lais VERSION 3.6.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(Threads REQUIRED)

add_executable(lais
    src/common.cpp
    src/telemetry.cpp
    src/crypto_pake.cpp
    src/coordinator.cpp
    src/http_server.cpp
    src/engine_manager.cpp
    src/main.cpp
    src/line_editor.cpp
)

target_include_directories(lais PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

set_target_properties(lais PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

if(MSVC)
    target_compile_options(lais PRIVATE /O2 /DNDEBUG /EHsc /utf-8 /MP /W3)
else()
    target_compile_options(lais PRIVATE
        -O3
        -DNDEBUG
        -fno-plt
        -Wall
        -Wextra
        -Wno-unused-result
    )
endif()

include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error)
if(ipo_supported)
    set_property(TARGET lais PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

if(WIN32)
    target_link_libraries(lais PRIVATE Threads::Threads ws2_32 psapi advapi32)
else()
    target_link_libraries(lais PRIVATE Threads::Threads)
endif()

include(GNUInstallDirs)
install(TARGETS lais RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY configs/ DESTINATION ${CMAKE_INSTALL_DATADIR}/lais/configs)
install(DIRECTORY data/ DESTINATION ${CMAKE_INSTALL_DATADIR}/lais/data)
