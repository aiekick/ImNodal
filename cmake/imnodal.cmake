
set(IMNODAL_DIR ${CMAKE_SOURCE_DIR}/3rdparty/imnodal)
add_subdirectory(${IMNODAL_DIR})

target_include_directories(ImNodal PUBLIC ${IMNODAL_DIR})

target_link_libraries(ImNodal PRIVATE imgui)
    
set_target_properties(ImNodal PROPERTIES FOLDER 3rdparty)
