cmake_minimum_required(VERSION 3.15)
project(vibration-green)

include(FetchContent)
FetchContent_Declare(
    detours
    GIT_REPOSITORY https://github.com/microsoft/Detours.git
    GIT_TAG        v4.0.1
)
FetchContent_MakeAvailable(detours)

# Build Detours as a static library
file(GLOB DETOURS_SRCS "${detours_SOURCE_DIR}/src/*.cpp")
list(FILTER DETOURS_SRCS EXCLUDE REGEX "uimports\\.cpp$")
add_library(detours STATIC ${DETOURS_SRCS})
target_include_directories(detours PUBLIC "${detours_SOURCE_DIR}/src")
target_compile_definitions(detours PRIVATE WIN32_LEAN_AND_MEAN)

add_library(autogreen SHARED dllmain.cpp)
target_link_libraries(autogreen PRIVATE xinput detours)

add_executable(injector injector.cpp)
