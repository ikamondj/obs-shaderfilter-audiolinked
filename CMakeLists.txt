# --- Detect if the plugin is build out of tree or not ---
if(CMAKE_PROJECT_NAME STREQUAL "obs-studio")
  set(BUILD_OUT_OF_TREE OFF)
else()
  set(BUILD_OUT_OF_TREE ON)
  cmake_minimum_required(VERSION 3.18)
endif()

project(obs-shaderfilter VERSION 2.6.0)
set(PROJECT_FULL_NAME "OBS Shaderfilter")

# Set new UUIDs when you start to create a new plugin.
set(MACOS_PACKAGE_UUID "7DA301DD-35F4-44C3-BE34-B33528D487F5")
set(MACOS_INSTALLER_UUID "D1247630-89C0-498A-8A3B-B0752080DEC2")

# TODO: Benchmark a production-grade SIMD IIR implementation before replacing
# the small built-in audio crossover. If it is materially faster or more
# accurate, package it through vcpkg or Conan instead of fetching it here.

add_library(${PROJECT_NAME} MODULE)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets)
# SIMD real FFT, provided by the adjacent vcpkg manifest. Keep its static
# implementation private to the plugin (no additional runtime DLL).
find_package(pffft CONFIG REQUIRED)
find_package(Threads REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE pffft::pffft Threads::Threads)
if(NOT WIN32)
  target_link_libraries(${PROJECT_NAME} PRIVATE m)
endif()
# vcpkg (manifest alongside this file) and Conan CMakeDeps both provide glfw3.
find_package(glfw3 3.4 QUIET CONFIG)
option(SHADERFILTER_FETCH_GLFW "Fetch pinned GLFW when no package is installed" ON)
if(NOT TARGET glfw AND NOT TARGET glfw3::glfw)
  if(NOT SHADERFILTER_FETCH_GLFW)
    message(FATAL_ERROR "GLFW 3.4 is required. Use vcpkg/Conan or enable SHADERFILTER_FETCH_GLFW.")
  endif()
  include(FetchContent)
  set(GLFW_BUILD_EXAMPLES OFF)
  set(GLFW_BUILD_TESTS OFF)
  set(GLFW_BUILD_DOCS OFF)
  set(GLFW_INSTALL OFF)
  set(GLFW_LIBRARY_TYPE STATIC)
  FetchContent_Declare(shaderfilter_glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 7b6aead9fb88b3623e3b3725ebb42670cbe4c579) # GLFW 3.4
  FetchContent_MakeAvailable(shaderfilter_glfw)
  set_target_properties(glfw PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()
if(TARGET glfw)
  set(SHADERFILTER_GLFW_TARGET glfw)
else()
  set(SHADERFILTER_GLFW_TARGET glfw3::glfw)
endif()
target_link_libraries(${PROJECT_NAME} PRIVATE ${SHADERFILTER_GLFW_TARGET})
target_link_libraries(${PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets OBS::frontend-api)
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_17)

foreach(part IN ITEMS shader-helpers gamepad spectrum)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "data/internal/${part}.effect")
  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/data/internal/${part}.effect" text)
  string(REPLACE "\\" "\\\\" text "${text}")
  string(REPLACE "\"" "\\\"" text "${text}")
  string(REPLACE "\n" "\\n\"\n\"" text "${text}")
  if(part STREQUAL "gamepad")
    set(SHADER_GAMEPAD "${text}")
  elseif(part STREQUAL "spectrum")
    set(SHADER_SPECTRUM "${text}")
  else()
    set(SHADER_HELPERS "${text}")
  endif()
endforeach()
configure_file(cmake/shader-helpers.h.in shader-helpers.generated.h @ONLY)
target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/version.h.in ${CMAKE_CURRENT_SOURCE_DIR}/version.h)

target_sources(${PROJECT_NAME} PRIVATE
	obs-shaderfilter.c
	shader-gamepad.cpp
	shader-gamepad.h
	shader-gamepad-state.hpp
	shader-audio-energy.h
	shader-spectrum.cpp
	shader-spectrum.h
	shader-spectrum-effect.h
	shader-tools.cpp
	shader-tools.h
	shader-controller-dialog.cpp
	shader-controller-dialog.hpp
	"${CMAKE_CURRENT_BINARY_DIR}/shader-helpers.generated.h"
	version.h)

if(BUILD_OUT_OF_TREE)
  find_package(libobs REQUIRED)
  find_package(obs-frontend-api REQUIRED)
  include(cmake/ObsPluginHelpers.cmake)
endif()

if(OS_WINDOWS)
	get_filename_component(ISS_FILES_DIR "${CMAKE_BINARY_DIR}\\..\\package" ABSOLUTE)
	file(TO_NATIVE_PATH "${ISS_FILES_DIR}" ISS_FILES_DIR)
	get_filename_component(ISS_PACKAGE_DIR "${CMAKE_PACKAGE_PREFIX}\\.." ABSOLUTE)
	file(TO_NATIVE_PATH "${ISS_PACKAGE_DIR}" ISS_PACKAGE_DIR)
	get_filename_component(ISS_SOURCE_DIR "${PROJECT_SOURCE_DIR}" ABSOLUTE)
	file(TO_NATIVE_PATH "${ISS_SOURCE_DIR}" ISS_SOURCE_DIR)
	configure_file("installer.iss.in"
			"${PROJECT_BINARY_DIR}/installer.iss"
		)
	configure_file(${CMAKE_CURRENT_SOURCE_DIR}/resource.rc.in ${CMAKE_CURRENT_SOURCE_DIR}/${PROJECT_NAME}.rc)
	target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/${PROJECT_NAME}.rc)

elseif(OS_MACOS)
	set(MACOS_BUNDLEID "com.exeldro.${PROJECT_NAME}")
	set(MACOSX_PLUGIN_GUI_IDENTIFIER "${MACOS_BUNDLEID}")
	set(MACOSX_PLUGIN_BUNDLE_VERSION "${PROJECT_VERSION}")
	set(MACOSX_PLUGIN_SHORT_VERSION_STRING "1")
	configure_file(cmake/bundle/macos/installer-macos.pkgproj.in ${CMAKE_BINARY_DIR}/installer-macos.generated.pkgproj)
	target_compile_options(${PROJECT_NAME} PRIVATE -Wall)
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE
		OBS::libobs)

option(SHADERFILTER_BUILD_TESTS "Build shaderfilter audio, controller and GPU helper tests" OFF)
if(SHADERFILTER_BUILD_TESTS)
  enable_testing()
  add_executable(shaderfilter-spectrum-test tests/spectrum-test.cpp shader-spectrum.cpp)
  target_compile_features(shaderfilter-spectrum-test PRIVATE cxx_std_17)
  target_include_directories(shaderfilter-spectrum-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(shaderfilter-spectrum-test PRIVATE pffft::pffft Threads::Threads)
  if(NOT WIN32)
    target_link_libraries(shaderfilter-spectrum-test PRIVATE m)
  endif()
  add_test(NAME shaderfilter-spectrum COMMAND shaderfilter-spectrum-test)
  add_executable(shaderfilter-audio-energy-test tests/audio-energy-test.c)
  target_include_directories(shaderfilter-audio-energy-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  if(NOT WIN32)
    target_link_libraries(shaderfilter-audio-energy-test PRIVATE m)
  endif()
  add_test(NAME shaderfilter-audio-energy COMMAND shaderfilter-audio-energy-test)
  add_executable(shaderfilter-gamepad-test tests/gamepad-state-test.cpp)
  target_compile_features(shaderfilter-gamepad-test PRIVATE cxx_std_17)
  target_include_directories(shaderfilter-gamepad-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  add_test(NAME shaderfilter-gamepad-state COMMAND shaderfilter-gamepad-test)
  add_executable(shaderfilter-helpers-test tests/shader-helpers-test.cpp)
  target_compile_features(shaderfilter-helpers-test PRIVATE cxx_std_17)
  target_include_directories(shaderfilter-helpers-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}")
  target_link_libraries(shaderfilter-helpers-test PRIVATE OBS::libobs)
  add_executable(shaderfilter-gamepad-api-test tests/gamepad-api-test.cpp shader-gamepad.cpp)
  target_compile_features(shaderfilter-gamepad-api-test PRIVATE cxx_std_17)
  target_include_directories(shaderfilter-gamepad-api-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
    "$<TARGET_PROPERTY:${SHADERFILTER_GLFW_TARGET},INTERFACE_INCLUDE_DIRECTORIES>")
  target_link_libraries(shaderfilter-gamepad-api-test PRIVATE Qt6::Core OBS::libobs)
  add_test(NAME shaderfilter-gamepad-api COMMAND shaderfilter-gamepad-api-test)
  add_executable(shaderfilter-tools-test tests/shader-tools-test.cpp shader-tools.cpp shader-controller-dialog.cpp)
  target_compile_features(shaderfilter-tools-test PRIVATE cxx_std_17)
  target_include_directories(shaderfilter-tools-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
    "$<TARGET_PROPERTY:OBS::frontend-api,INTERFACE_INCLUDE_DIRECTORIES>")
  target_link_libraries(shaderfilter-tools-test PRIVATE Qt6::Widgets OBS::libobs)
  add_test(NAME shaderfilter-tools COMMAND shaderfilter-tools-test)
  set_tests_properties(shaderfilter-spectrum shaderfilter-audio-energy shaderfilter-gamepad-state shaderfilter-gamepad-api shaderfilter-tools PROPERTIES TIMEOUT 30)
endif()

if(BUILD_OUT_OF_TREE)
    if(NOT LIB_OUT_DIR)
        set(LIB_OUT_DIR "/lib/obs-plugins")
    endif()
    if(NOT DATA_OUT_DIR)
        set(DATA_OUT_DIR "/share/obs/obs-plugins/${PROJECT_NAME}")
    endif()
    set_target_properties(${PROJECT_NAME} PROPERTIES PREFIX "")
    install(TARGETS ${PROJECT_NAME}
        LIBRARY DESTINATION ${CMAKE_INSTALL_PREFIX}/${LIB_OUT_DIR})
    install(DIRECTORY data/examples data/internal data/locale data/textures
        DESTINATION ${CMAKE_INSTALL_PREFIX}/${DATA_OUT_DIR})
    setup_plugin_target(${PROJECT_NAME})
else()
    if(OBS_CMAKE_VERSION VERSION_GREATER_EQUAL 3.0.0)
        set_target_properties_obs(${PROJECT_NAME} PROPERTIES FOLDER "plugins/exeldro" PREFIX "")
    else()
        set_target_properties(${PROJECT_NAME} PROPERTIES FOLDER "plugins/exeldro")
        setup_plugin_target(${PROJECT_NAME})
    endif()
endif()
