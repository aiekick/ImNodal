add_subdirectory(${CMAKE_SOURCE_DIR}/ImNodal)

set_target_properties(ImNodal PROPERTIES LINKER_LANGUAGE CXX)
set_target_properties(ImNodal PROPERTIES FOLDER 3rdparty)

set(IMNODAL_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/ImNodal)
set(IMNODAL_LIBRARIES ImNodal)

