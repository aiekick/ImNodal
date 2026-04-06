cmake_minimum_required(VERSION 3.20)
project(ImNodal)

# Option to control installation (disabled by default if used as subproject)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(IM_NODAL_INSTALL_DEFAULT ON)
else()
    set(IM_NODAL_INSTALL_DEFAULT OFF)
endif()

option(IM_NODAL_INSTALL "Install ImNodal library and headers" ${IM_NODAL_INSTALL_DEFAULT})

find_package(imgui QUIET) # fails quietly if not found (imgui is the target name for vcpkg and nixpkgs).

add_library(ImNodal
    ImNodal.cpp
    ImNodal.h
    ImNodalConfig.h
)

if(imgui_FOUND)
    target_link_libraries(ImNodal PUBLIC imgui::imgui)
    message(STATUS "ImNodal: Found imgui package, linking automatically")
else()
    message(STATUS "ImNodal: imgui package not found - ensure ImGui is available in your project")
endif()

# Use generator expressions for proper include directory handling
target_include_directories(ImNodal
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:include>
)

if(UNIX)
    target_compile_options(ImNodal PUBLIC -Wno-unknown-pragmas)
endif()

# Installation configuration
if(IM_NODAL_INSTALL)
    include(GNUInstallDirs)

    # Install the library
    install(TARGETS ImNodal
       EXPORT ImNodalTargets
       ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
       LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
       RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )

    # Install headers
    install(FILES 
       ImNodal.h 
       ImNodalConfig.h
       DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )

    # Install export targets
    install(EXPORT ImNodalTargets
       FILE ImNodalConfig.cmake
       NAMESPACE ImNodal::
       DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ImNodal
    )
endif()