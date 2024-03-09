cmake_minimum_required(VERSION 3.5)

project(ImNodal)

add_library(ImNodal STATIC
    ImNodal.cpp
    ImNodal.h
    ImNodalConfig.h
)

target_include_directories(ImNodal PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(UNIX)
    target_compile_options(ImNodal PUBLIC -Wno-unknown-pragmas)
endif()
