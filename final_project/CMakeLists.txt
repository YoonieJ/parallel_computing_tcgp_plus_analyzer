cmake_minimum_required(VERSION 3.16)

project(TCGPPlus LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(OpenMP REQUIRED)

add_executable(TCGPPlus
    main.cpp
    FileParser.cpp
    GameSimulation.cpp
)

target_include_directories(TCGPPlus PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(TCGPPlus PRIVATE OpenMP::OpenMP_CXX)
