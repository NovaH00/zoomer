cmake_minimum_required(VERSION 3.20)

project(zoomer
    VERSION 0.0.1
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_COMPILER g++-14)

set(SRCS
    src/main.cpp
)

# Executable
add_executable(${PROJECT_NAME} ${SRCS})

find_package(PkgConfig REQUIRED)
pkg_check_modules(GIO REQUIRED gio-2.0)

find_package(X11 REQUIRED COMPONENTS Xrandr)
find_package(SDL2 REQUIRED)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${GIO_INCLUDE_DIRS}
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    X11::X11
    X11::Xrandr
    SDL2::SDL2-static
    ${GIO_LIBRARIES}
)

# Compiler warnings
target_compile_options(${PROJECT_NAME} PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)
