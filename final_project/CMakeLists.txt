cmake_minimum_required(VERSION 3.16)

project(TCGPPlus LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(OpenMP)

add_executable(TCGPPlus
    main.cpp
    FileParser.cpp
    GameSimulation.cpp
)

target_include_directories(TCGPPlus PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

if(OpenMP_CXX_FOUND)
    target_link_libraries(TCGPPlus PRIVATE OpenMP::OpenMP_CXX)
else()
    message(WARNING "OpenMP was not found. TCGPPlus will build with the portable thread fallback.")
endif()
