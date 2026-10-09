cmake_minimum_required(VERSION 3.15)
project(vibration-green)

# Detours (local copy)
add_library(detours STATIC
    detours/detours.cpp
    detours/modules.cpp
    detours/disasm.cpp
    detours/image.cpp
    detours/creatwth.cpp
    detours/disolx64.cpp
    detours/disolx86.cpp
    detours/disolia64.cpp
    detours/disolarm.cpp
    detours/disolarm64.cpp
)
target_include_directories(detours PUBLIC detours)
target_compile_definitions(detours PRIVATE WIN32_LEAN_AND_MEAN)

add_library(autogreen SHARED dllmain.cpp)
target_link_libraries(autogreen PRIVATE xinput detours)

add_executable(injector injector.cpp)
