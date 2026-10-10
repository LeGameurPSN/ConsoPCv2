cmake_minimum_required(VERSION 3.20)
project(PowerMon VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
    message(FATAL_ERROR "PowerMon est un outil Windows uniquement.")
endif()

add_executable(powermon WIN32
    src/main.cpp
)

set_target_properties(powermon PROPERTIES
    OUTPUT_NAME "powermon"
)

target_link_libraries(powermon PRIVATE
    d2d1
    dwrite
    pdh
    wbemuuid
    ole32
    oleaut32
    setupapi
    powrprof
    psapi
    dxgi
    iphlpapi
    comctl32
)

# /MT = runtime statique → zéro dépendance sur vc_redist
target_compile_options(powermon PRIVATE
    $<$<CONFIG:Release>:/O2 /GL /MT /DNDEBUG>
    $<$<CONFIG:Debug>:/Od /Zi /MTd>
)

target_link_options(powermon PRIVATE
    $<$<CONFIG:Release>:/LTCG /SUBSYSTEM:WINDOWS>
    $<$<CONFIG:Debug>:/SUBSYSTEM:WINDOWS>
)

# Manifeste DPI awareness
target_sources(powermon PRIVATE powermon.manifest)
