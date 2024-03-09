cmake_minimum_required(VERSION 3.1)

set(PROJECT ImNodalApp)

enable_language(C CXX)
project(${PROJECT} CXX)

set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -D_DEBUG")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -D_DEBUG")
set(FINAL_BIN_DIR ${CMAKE_SOURCE_DIR}/bin)
set(CMAKE_PDB_OUTPUT_DIRECTORY "${FINAL_BIN_DIR}/debug")
if(NOT CMAKE_DEBUG_POSTFIX)
  set(CMAKE_DEBUG_POSTFIX _debug)
endif()

if (UNIX)
	option(USE_LEAK_SANITIZER "Enable the Leak Sanitizer" OFF)
endif()

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(cmake/3rdparty.cmake)

add_definitions(${GLFW_DEFINITIONS})
add_definitions(-DIMGUI_DISABLE_OBSOLETE_FUNCTIONS)
add_definitions(-DCUSTOM_IMGUIFILEDIALOG_CONFIG="${CMAKE_CURRENT_SOURCE_DIR}/CustomImGuiFileDialogConfig.h")
add_definitions(-DCUSTOM_IMWIDGETS_CONFIG="${CMAKE_CURRENT_SOURCE_DIR}/CustomImWidgetsConfig.h")
add_definitions(-D_CRT_SECURE_NO_WARNINGS)

#################################################
#### SOURCES ####################################
#################################################

file(GLOB MAIN_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp)
file(GLOB IMGUI_IMPL_SOURCES 
	${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/imgui/backends/imgui_impl_glfw.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/imgui/backends/imgui_impl_glfw.h
	${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/imgui/backends/imgui_impl_opengl3.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/imgui/backends/imgui_impl_opengl3.h)
file(GLOB_RECURSE CMAKE_SOURCES
	${CMAKE_SOURCE_DIR}/cmake/*.cmake)
file(GLOB_RECURSE SRC_SOURCES
	${CMAKE_SOURCE_DIR}/src/*.cpp
	${CMAKE_SOURCE_DIR}/src/*.h
	${CMAKE_SOURCE_DIR}/src/*.hpp)
	
source_group(main FILES ${MAIN_SOURCES})
source_group(src/impl FILES ${IMGUI_IMPL_SOURCES})
source_group(TREE ${CMAKE_SOURCE_DIR}/cmake PREFIX cmake FILES ${CMAKE_SOURCES})
source_group(TREE ${CMAKE_SOURCE_DIR}/src PREFIX src FILES ${SRC_SOURCES})

#################################################
#### PRE INCLUDES ###############################
#################################################

include_directories(
	${GLM_INCLUDE_DIR}
	${PLOG_INCLUDE_DIR}
	${GLFW_INCLUDE_DIR}
	${GLAD_INCLUDE_DIR}
	${IMGUI_INCLUDE_DIR}
	${OPENGL_INCLUDE_DIR}
	${IMWIDGETS_INCLUDE_DIR}
	${CMAKE_CURRENT_SOURCE_DIR}
)

#################################################
#### LEAK #######################################
#################################################

if (USE_LEAK_SANITIZER)
	add_compile_options(-fexceptions -pthread -ldl -lGL -Wall -g -fsanitize=undefined -fno-sanitize-recover -fsanitize=float-cast-overflow -fsanitize=leak -fsanitize=address -fsanitize-address-use-after-scope -fstack-protector -fstack-protector-all)
	add_link_options(-fexceptions -pthread -ldl -lGL -Wall -g -fsanitize=undefined -fno-sanitize-recover -fsanitize=float-cast-overflow -fsanitize=leak -fsanitize=address -fsanitize-address-use-after-scope -fstack-protector -fstack-protector-all)
endif()

#################################################
#### EXECUTABLE #################################
#################################################

add_executable(${PROJECT}
	${MAIN_SOURCES}
	${IMGUI_IMPL_SOURCES}
	${CMAKE_SOURCES}
	${SRC_SOURCES}
)

#################################################
#### COMPILE OPTIONS ############################
#################################################

set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(UNIX)
    target_compile_options(${PROJECT} PUBLIC -Wno-unknown-pragmas)
endif()

target_link_libraries(${PROJECT} PRIVATE
	${OPENGL_LIBRARIES}
	${GLFW_LIBRARIES}
	${GLAD_LIBRARIES}
	${IMGUI_LIBRARIES}
	${IMWIDGETS_LIBRARIES}
	ImGuiFileDialog
)
	
if (USE_TEST)
	enable_testing()
	add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/tests)
endif()

set_target_properties(${PROJECT} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${FINAL_BIN_DIR}")
set_target_properties(${PROJECT} PROPERTIES RUNTIME_OUTPUT_DIRECTORY_DEBUG "${FINAL_BIN_DIR}")
set_target_properties(${PROJECT} PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${FINAL_BIN_DIR}")
