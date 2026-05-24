cmake_minimum_required(VERSION 3.20)

set(PROJECT ImNodalDemo)

project(${PROJECT} LANGUAGES C CXX)

option(USE_BUILDING_OF_TESTS "Use building of tests" OFF)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -D_DEBUG")

set(FINAL_BIN_DIR ${CMAKE_SOURCE_DIR}/bin)
set(CMAKE_PDB_OUTPUT_DIRECTORY_DEBUG "${FINAL_BIN_DIR}/debug")
set(CMAKE_PDB_OUTPUT_DIRECTORY_RELEASE "${FINAL_BIN_DIR}/release")
set(CMAKE_PDB_OUTPUT_DIRECTORY_MINSIZEREL "${FINAL_BIN_DIR}/minsizerel")
set(CMAKE_PDB_OUTPUT_DIRECTORY_RELWITHDEBINFO "${FINAL_BIN_DIR}/reldeb")

set(CMAKE_INSTALL_PREFIX .)
set(CMAKE_INSTALL_BINDIR .)

if(NOT CMAKE_DEBUG_POSTFIX)
    set(CMAKE_DEBUG_POSTFIX _debug)
endif()
if(NOT CMAKE_RELEASE_POSTFIX)
    set(CMAKE_RELEASE_POSTFIX) ## to config in file
endif()
if(NOT CMAKE_MINSIZEREL_POSTFIX)
    set(CMAKE_MINSIZEREL_POSTFIX _minsizerel)
endif()
if(NOT CMAKE_RELWITHDEBINFO_POSTFIX)
    set(CMAKE_RELWITHDEBINFO_POSTFIX _reldeb)
endif()

if(MSVC)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /Zo")
    add_definitions(
		-D_USE_MATH_DEFINES
		-DMSVC
		-DNOMINMAX
		-D_CRT_SECURE_NO_WARNINGS
	)
else ()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -m64 -Wall -Wextra -Wpedantic -Wunused-parameter -Wno-unknown-pragmas")
endif ()

set(OpenGL_GL_PREFERENCE GLVND)
find_package(OpenGL REQUIRED)

include(FetchContent)

include(cmake/glfw.cmake)
include(cmake/imgui.cmake)
include(cmake/imnodal.cmake)

file(GLOB_RECURSE SRC_SOURCES ${CMAKE_SOURCE_DIR}/src/*.*)
source_group(TREE ${CMAKE_SOURCE_DIR}/src PREFIX src FILES ${SRC_SOURCES})

file(GLOB IMGUI_IMPL_SOURCES 
	${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
	${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.h
	${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
	${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.h)
source_group(src/imgui FILES ${IMGUI_IMPL_SOURCES})

add_executable(${PROJECT}
	${SRC_SOURCES}
	${IMGUI_IMPL_SOURCES}
)

target_include_directories(${PROJECT} PUBLIC 
	${imgui_SOURCE_DIR}
	${CMAKE_SOURCE_DIR}/src
	${CMAKE_SOURCE_DIR}/3rdparty/imnodal
)

target_link_libraries(${PROJECT} PRIVATE ImNodal glfw)

# Platform-specific libraries
if(UNIX AND NOT APPLE)
    target_link_libraries(${PROJECT} PRIVATE dl pthread GL)
elseif(APPLE)
    target_link_libraries(${PROJECT} PRIVATE "-framework OpenGL" "-framework Cocoa" "-framework IOKit" "-framework CoreVideo")
elseif(WIN32)
    target_link_libraries(${PROJECT} PRIVATE opengl32)
endif()

set_target_properties(${PROJECT} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${FINAL_BIN_DIR})
set_target_properties(${PROJECT} PROPERTIES RUNTIME_OUTPUT_DIRECTORY_DEBUG ${FINAL_BIN_DIR})
set_target_properties(${PROJECT} PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE ${FINAL_BIN_DIR})
set_target_properties(${PROJECT} PROPERTIES RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL ${FINAL_BIN_DIR})
set_target_properties(${PROJECT} PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${FINAL_BIN_DIR})

set_target_properties(${PROJECT} PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "${FINAL_BIN_DIR}")

set_property(TARGET ${PROJECT} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
