set(TINYXML2_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/tinyxml2)

file(GLOB TINYXML2_FILES 
	${TINYXML2_INCLUDE_DIR}/tinyxml2.h 
	${TINYXML2_INCLUDE_DIR}/tinyxml2.cpp
)

add_library(tinyxml2 STATIC ${TINYXML2_FILES})

set_target_properties(tinyxml2 PROPERTIES LINKER_LANGUAGE CXX)
set_target_properties(tinyxml2 PROPERTIES FOLDER 3rdparty)

set(TINYXML2_LIBRARIES tinyxml2)
