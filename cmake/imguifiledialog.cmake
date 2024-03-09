add_subdirectory(${CMAKE_SOURCE_DIR}/3rdparty/ImGuiFileDialog)

set_target_properties(ImGuiFileDialog PROPERTIES LINKER_LANGUAGE CXX)
set_target_properties(ImGuiFileDialog PROPERTIES FOLDER 3rdparty)

set(IGFD_LIBRARIES ImGuiFileDialog)

