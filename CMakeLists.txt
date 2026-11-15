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

find_package(X11 REQUIRED COMPONENTS Xrandr)
find_package(SDL2 REQUIRED)

target_link_libraries(${PROJECT_NAME} PRIVATE
    X11::X11
    X11::Xrandr
    SDL2::SDL2-static
)

# Compiler warnings
target_compile_options(${PROJECT_NAME} PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)
