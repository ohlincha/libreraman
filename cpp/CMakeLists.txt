cmake_minimum_required(VERSION 3.21)
if(POLICY CMP0144)
  cmake_policy(SET CMP0144 NEW)        # use <PACKAGENAME>_ROOT hints (LIBUSB_ROOT, HIDAPI_ROOT)
endif()
project(LibreRamen VERSION 0.3.7 LANGUAGES CXX)

# C++ implementation of LibreRamen (the Python package in ../libreramen is the reference).
# Layout:  src/core  engine library (Qt Core + SerialPort, libusb, hidapi, Eigen; no widgets)
#          src/cli   command line          src/gui  Qt Widgets application
#          src/mcp   MCP server for AI agents (client of the application's local API)
#          tests     QtTest unit tests, incl. golden data produced by the Python reference

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

option(LIBRERAMEN_GUI "Build the Qt Widgets application" ON)
option(LIBRERAMEN_TESTS "Build the unit tests" ON)

find_package(Qt6 6.2 REQUIRED COMPONENTS Core SerialPort Network Sql)
if(LIBRERAMEN_GUI)
  find_package(Qt6 REQUIRED COMPONENTS Gui Widgets Svg)
endif()
find_package(Eigen3 REQUIRED NO_MODULE)       # 3.4 (Debian/Ubuntu) or 5.x (vcpkg, Homebrew)
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
target_link_libraries(libreramen_core PUBLIC Qt6::Core Qt6::SerialPort Qt6::Network Qt6::Sql Eigen3::Eigen
                      LibreRamen::libusb LibreRamen::hidapi)
if(WIN32)
  target_link_libraries(libreramen_core PUBLIC setupapi shell32)
endif()

# ---- command line
file(GLOB CLI_SOURCES CONFIGURE_DEPENDS src/cli/*.cpp src/cli/*.h)
add_executable(libreramen-cli ${CLI_SOURCES})
target_link_libraries(libreramen-cli PRIVATE libreramen_core)
set_target_properties(libreramen-cli PROPERTIES OUTPUT_NAME LibreRamenCLI)

# ---- MCP server (stdio) for AI agents
add_executable(libreramen-mcp src/mcp/main.cpp)
target_link_libraries(libreramen-mcp PRIVATE libreramen_core)
set_target_properties(libreramen-mcp PROPERTIES OUTPUT_NAME LibreRamenMCP)

# ---- GUI
# src/gui (without main.cpp) is an OBJECT library so that the application and the widget tests
# (tests/gui_test_*.cpp) share the compiled widgets and the gui.qrc resources (no Q_INIT_RESOURCE needed).
if(LIBRERAMEN_GUI)
  file(GLOB_RECURSE GUI_SOURCES CONFIGURE_DEPENDS src/gui/*.cpp src/gui/*.h src/gui/*.ui)
  list(FILTER GUI_SOURCES EXCLUDE REGEX "/src/gui/main\\.cpp$")
  add_library(libreramen_gui OBJECT ${GUI_SOURCES} resources/gui.qrc resources/help.qrc)
  target_link_libraries(libreramen_gui PUBLIC libreramen_core Qt6::Gui Qt6::Widgets Qt6::Svg)
  add_executable(libreramen WIN32 MACOSX_BUNDLE src/gui/main.cpp)
  target_link_libraries(libreramen PRIVATE libreramen_gui)
  set_target_properties(libreramen PROPERTIES OUTPUT_NAME LibreRamen
    MACOSX_BUNDLE_GUI_IDENTIFIER io.github.ohlincha.libreramen
    MACOSX_BUNDLE_BUNDLE_NAME LibreRamen
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION})

  # tools/gui_preview renders the widgets to PNG files (for looking at them); not built by default
  option(LIBRERAMEN_GUI_PREVIEW "Build tools/gui_preview" OFF)
  if(LIBRERAMEN_GUI_PREVIEW)
    add_executable(gui_preview tools/gui_preview.cpp)
    target_link_libraries(gui_preview PRIVATE libreramen_gui)
    add_executable(datapage_preview tools/datapage_preview.cpp)
    target_link_libraries(datapage_preview PRIVATE libreramen_gui)
    add_executable(calibration_preview tools/calibration_preview.cpp)
    target_link_libraries(calibration_preview PRIVATE libreramen_gui)
    add_executable(computed_preview tools/computed_preview.cpp)
    target_link_libraries(computed_preview PRIVATE libreramen_gui)
    target_compile_definitions(computed_preview PRIVATE GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/golden")
  endif()
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
  # test_cli runs the command line program
  target_compile_definitions(test_cli PRIVATE CLI_PATH="$<TARGET_FILE:libreramen-cli>")
  add_dependencies(test_cli libreramen-cli)
  if(LIBRERAMEN_GUI)                       # widget tests: also link the GUI object library
    file(GLOB GUI_TEST_SOURCES CONFIGURE_DEPENDS tests/gui_test_*.cpp)
    foreach(src ${GUI_TEST_SOURCES})
      get_filename_component(name ${src} NAME_WE)
      add_executable(${name} ${src})
      target_link_libraries(${name} PRIVATE libreramen_gui Qt6::Test)
      target_compile_definitions(${name} PRIVATE GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/golden")
      add_test(NAME ${name} COMMAND ${name})
      set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endforeach()
    # gui_test_api also runs the command line (--remote) and the MCP server against the API
    target_compile_definitions(gui_test_api PRIVATE CLI_PATH="$<TARGET_FILE:libreramen-cli>"
                               MCP_PATH="$<TARGET_FILE:libreramen-mcp>")
    add_dependencies(gui_test_api libreramen-cli libreramen-mcp)
  endif()
endif()

# ---- installation, platform resources and packages
include(cmake/Install.cmake)
