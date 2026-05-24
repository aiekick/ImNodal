
FetchContent_Declare(imgui
    URL ${CMAKE_SOURCE_DIR}/3rdparty/imgui-1.92.8-docking.tar.gz
	DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(imgui)

file(GLOB IMGUI_SOURCES ${imgui_SOURCE_DIR}/*.cpp)
file(GLOB IMGUI_HEADERS ${imgui_SOURCE_DIR}/*.h)
                 
add_library(imgui STATIC ${IMGUI_SOURCES} ${IMGUI_HEADERS})

add_definitions(-DIMGUI_DISABLE_OBSOLETE_FUNCTIONS)
    
set_target_properties(imgui PROPERTIES LINKER_LANGUAGE CXX)
set_target_properties(imgui PROPERTIES FOLDER 3rdparty)

target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR})
