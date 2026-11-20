cmake_minimum_required(VERSION 3.21)
project(LibreRamen VERSION 0.2.0 LANGUAGES CXX)

# C++ implementation of LibreRamen (the Python package in ../libreramen is the reference).
# Layout:  src/core  engine library (Qt Core + SerialPort, libusb, hidapi, Eigen; no widgets)
#          src/cli   command line          src/gui  Qt Widgets application
#          tests     QtTest unit tests, incl. golden data produced by the Python reference

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

option(LIBRERAMEN_GUI "Build the Qt Widgets application" ON)
option(LIBRERAMEN_TESTS "Build the unit tests" ON)

find_package(Qt6 6.2 REQUIRED COMPONENTS Core SerialPort Network)
if(LIBRERAMEN_GUI)
  find_package(Qt6 REQUIRED COMPONENTS Gui Widgets Svg)
endif()
find_package(Eigen3 3.3 REQUIRED NO_MODULE)
find_package(PkgConfig)
include(cmake/Deps.cmake)        # libusb-1.0 and hidapi (pkg-config, vcpkg or explicit paths)

add_compile_definitions(LIBRERAMEN_VERSION="${PROJECT_VERSION}")
if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8)
  add_compile_definitions(_USE_MATH_DEFINES NOMINMAX)
else()
  add_compile_options(-Wall -Wextra)
endif()

# ---- engine
file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS src/core/*.cpp src/core/*.h)
add_library(libreramen_core STATIC ${CORE_SOURCES} resources/core.qrc)
target_include_directories(libreramen_core PUBLIC src)
target_link_libraries(libreramen_core PUBLIC Qt6::Core Qt6::SerialPort Qt6::Network Eigen3::Eigen
                      LibreRamen::libusb LibreRamen::hidapi)
if(WIN32)
  target_link_libraries(libreramen_core PUBLIC setupapi shell32)
endif()

# ---- command line
file(GLOB CLI_SOURCES CONFIGURE_DEPENDS src/cli/*.cpp src/cli/*.h)
add_executable(libreramen-cli ${CLI_SOURCES})
target_link_libraries(libreramen-cli PRIVATE libreramen_core)
set_target_properties(libreramen-cli PROPERTIES OUTPUT_NAME LibreRamenCLI)

# ---- GUI
if(LIBRERAMEN_GUI)
  file(GLOB_RECURSE GUI_SOURCES CONFIGURE_DEPENDS src/gui/*.cpp src/gui/*.h src/gui/*.ui)
  add_executable(libreramen WIN32 MACOSX_BUNDLE ${GUI_SOURCES} resources/gui.qrc)
  target_link_libraries(libreramen PRIVATE libreramen_core Qt6::Gui Qt6::Widgets Qt6::Svg)
  set_target_properties(libreramen PROPERTIES OUTPUT_NAME LibreRamen
    MACOSX_BUNDLE_GUI_IDENTIFIER io.github.ohlincha.libreramen
    MACOSX_BUNDLE_BUNDLE_NAME LibreRamen
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION})
endif()

# ---- tests
if(LIBRERAMEN_TESTS)
  enable_testing()
  find_package(Qt6 REQUIRED COMPONENTS Test)
  file(GLOB TEST_SOURCES CONFIGURE_DEPENDS tests/test_*.cpp)
  foreach(src ${TEST_SOURCES})
    get_filename_component(name ${src} NAME_WE)
    add_executable(${name} ${src})
    target_link_libraries(${name} PRIVATE libreramen_core Qt6::Test)
    target_compile_definitions(${name} PRIVATE GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/golden")
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
  endforeach()
endif()
