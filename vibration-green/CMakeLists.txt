cmake_minimum_required(VERSION 3.15)
project(vibration-green)

add_library(autogreen SHARED dllmain.cpp)
target_link_libraries(autogreen PRIVATE xinput psapi shell32)

add_executable(injector injector.cpp)
