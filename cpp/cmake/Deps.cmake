# libusb-1.0 and hidapi as imported targets LibreRamen::libusb / LibreRamen::hidapi.
# Order: vcpkg/CMake packages, then pkg-config, then LIBUSB_ROOT / HIDAPI_ROOT hints.

# libusb
find_package(libusb-1.0 CONFIG QUIET)            # (not common; vcpkg uses pkg-config)
if(PkgConfig_FOUND)
  pkg_check_modules(LIBUSB QUIET IMPORTED_TARGET libusb-1.0)
endif()
if(TARGET PkgConfig::LIBUSB)
  add_library(LibreRamen::libusb ALIAS PkgConfig::LIBUSB)
else()
  find_path(LIBUSB_INCLUDE_DIR libusb.h PATH_SUFFIXES libusb-1.0 HINTS ${LIBUSB_ROOT}/include)
  find_library(LIBUSB_LIBRARY NAMES usb-1.0 libusb-1.0 HINTS ${LIBUSB_ROOT}/lib)
  if(NOT LIBUSB_INCLUDE_DIR OR NOT LIBUSB_LIBRARY)
    message(FATAL_ERROR "libusb-1.0 not found (Debian/Ubuntu: apt install libusb-1.0-0-dev; macOS: brew install libusb; "
                        "Windows: vcpkg install libusb, or set LIBUSB_ROOT)")
  endif()
  add_library(LibreRamen::libusb UNKNOWN IMPORTED)
  set_target_properties(LibreRamen::libusb PROPERTIES IMPORTED_LOCATION ${LIBUSB_LIBRARY}
                        INTERFACE_INCLUDE_DIRECTORIES ${LIBUSB_INCLUDE_DIR})
endif()

# hidapi (Linux: the hidraw backend)
find_package(hidapi CONFIG QUIET)
if(TARGET hidapi::hidraw)
  add_library(LibreRamen::hidapi ALIAS hidapi::hidraw)
elseif(TARGET hidapi::hidapi)
  add_library(LibreRamen::hidapi ALIAS hidapi::hidapi)
else()
  if(PkgConfig_FOUND)
    pkg_check_modules(HIDAPI QUIET IMPORTED_TARGET hidapi-hidraw)
    if(NOT HIDAPI_FOUND)
      pkg_check_modules(HIDAPI QUIET IMPORTED_TARGET hidapi)
    endif()
  endif()
  if(TARGET PkgConfig::HIDAPI)
    add_library(LibreRamen::hidapi ALIAS PkgConfig::HIDAPI)
  else()
    find_path(HIDAPI_INCLUDE_DIR hidapi.h PATH_SUFFIXES hidapi HINTS ${HIDAPI_ROOT}/include)
    find_library(HIDAPI_LIBRARY NAMES hidapi-hidraw hidapi HINTS ${HIDAPI_ROOT}/lib)
    if(NOT HIDAPI_INCLUDE_DIR OR NOT HIDAPI_LIBRARY)
      message(FATAL_ERROR "hidapi not found (Debian/Ubuntu: apt install libhidapi-dev; macOS: brew install hidapi; "
                          "Windows: vcpkg install hidapi, or set HIDAPI_ROOT)")
    endif()
    add_library(LibreRamen::hidapi UNKNOWN IMPORTED)
    set_target_properties(LibreRamen::hidapi PROPERTIES IMPORTED_LOCATION ${HIDAPI_LIBRARY}
                          INTERFACE_INCLUDE_DIRECTORIES ${HIDAPI_INCLUDE_DIR})
  endif()
endif()
