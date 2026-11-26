# Installation rules, platform resources (icons, version info, Info.plist, desktop entry, udev rule) and
# CPack (.deb). Included at the end of ../CMakeLists.txt. See cpp/README.md, "Packaging".
#
#   Linux:   <prefix>/bin/LibreRaman, LibreRamanCLI; desktop entry, hicolor icons, AppStream metainfo,
#            udev rule (<prefix>/lib/udev/rules.d, i.e. /usr/lib/udev/rules.d for the .deb). `cpack` -> .deb
#   macOS:   <prefix>/LibreRaman.app (CLI in Contents/MacOS next to the GUI; then macdeployqt)
#   Windows: <prefix>/LibreRaman.exe, LibreRamanCLI.exe, libusb/hidapi DLLs, MSVC runtime (then windeployqt)

include(GNUInstallDirs)

set(LIBRERAMAN_APP_ID io.github.ohlincha.libreraman)
set(LIBRERAMAN_COPYRIGHT "Copyright (c) 2026 ohlincha and LibreRaman contributors. MIT License.")
set(LR_PKG ${CMAKE_CURRENT_SOURCE_DIR}/packaging)
set(LR_ICON_PNG ${CMAKE_CURRENT_SOURCE_DIR}/resources/icon.png)
set(LR_LICENSE ${CMAKE_CURRENT_SOURCE_DIR}/../LICENSE)
set(LR_NOTICES ${LR_PKG}/THIRD-PARTY-NOTICES.txt)

# mkicon: makes .ico / .icns / hicolor PNGs from resources/icon.png at build time (needs Qt Gui, so only
# with the GUI; on Windows the Qt DLLs must be on PATH while building, as for running the tests).
if(LIBRERAMAN_GUI)
  add_executable(mkicon tools/mkicon.cpp)
  target_link_libraries(mkicon PRIVATE Qt6::Gui)
endif()

# ---------------------------------------------------------------- Windows
if(WIN32)
  set(LR_RC_ICON "")
  if(TARGET mkicon)
    set(LR_ICO ${CMAKE_CURRENT_BINARY_DIR}/libreraman.ico)
    add_custom_command(OUTPUT ${LR_ICO} COMMAND mkicon ico ${LR_ICON_PNG} ${LR_ICO}
                       DEPENDS mkicon ${LR_ICON_PNG} COMMENT "Windows icon")
    add_custom_target(libreraman_ico DEPENDS ${LR_ICO})
    set(LR_RC_ICON "IDI_ICON1 ICON \"${LR_ICO}\"")
  endif()
  foreach(spec "libreraman|LibreRaman|LibreRaman Raman spectrometer software"
               "libreraman-cli|LibreRamanCLI|LibreRaman command line"
               "libreraman-mcp|LibreRamanMCP|LibreRaman MCP server")
    string(REPLACE "|" ";" spec "${spec}")
    list(GET spec 0 tgt)
    list(GET spec 1 LR_FILE_NAME)
    list(GET spec 2 LR_DESCRIPTION)
    if(TARGET ${tgt})
      set(rc ${CMAKE_CURRENT_BINARY_DIR}/${tgt}.rc)
      configure_file(${LR_PKG}/windows/libreraman.rc.in ${rc} @ONLY)
      target_sources(${tgt} PRIVATE ${rc})
      if(LR_ICO)
        set_source_files_properties(${rc} PROPERTIES OBJECT_DEPENDS ${LR_ICO})
        add_dependencies(${tgt} libreraman_ico)
      endif()
    endif()
  endforeach()

  install(TARGETS libreraman-cli libreraman-mcp RUNTIME DESTINATION .)
  if(TARGET libreraman)
    install(TARGETS libreraman RUNTIME DESTINATION .)
  endif()
  install(FILES ${LR_LICENSE} DESTINATION . RENAME LICENSE.txt)
  install(FILES ${LR_NOTICES} DESTINATION .)
  # MSVC runtime next to the programs (app-local; no vc_redist needed). The UCRT is part of Windows 10+.
  set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION .)
  set(CMAKE_INSTALL_UCRT_LIBRARIES FALSE)
  include(InstallRequiredSystemLibraries)
  # libusb / hidapi DLLs (vcpkg: <installed>/<triplet>/bin; or LIBUSB_ROOT/HIDAPI_ROOT/bin)
  set(lr_dll_hints ${LIBUSB_ROOT}/bin ${HIDAPI_ROOT}/bin ${LIBUSB_PREFIX}/bin ${HIDAPI_PREFIX}/bin)
  if(DEFINED _VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
    list(APPEND lr_dll_hints ${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin)
  endif()
  foreach(p ${CMAKE_PREFIX_PATH})
    list(APPEND lr_dll_hints ${p}/bin)
  endforeach()
  foreach(dll libusb-1.0.dll hidapi.dll)
    unset(lr_dll)
    find_file(lr_dll NAMES ${dll} HINTS ${lr_dll_hints} NO_DEFAULT_PATH NO_CACHE)
    if(lr_dll)
      install(FILES ${lr_dll} DESTINATION .)
    else()
      message(WARNING "${dll} not found: it will be missing from the installation (static library?)")
    endif()
  endforeach()

# ---------------------------------------------------------------- macOS
elseif(APPLE)
  if(TARGET libreraman)
    set(LR_ICNS ${CMAKE_CURRENT_BINARY_DIR}/LibreRaman.icns)
    add_custom_command(OUTPUT ${LR_ICNS} COMMAND mkicon icns ${LR_ICON_PNG} ${LR_ICNS}
                       DEPENDS mkicon ${LR_ICON_PNG} COMMENT "macOS icon")
    target_sources(libreraman PRIVATE ${LR_ICNS})
    set_source_files_properties(${LR_ICNS} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    set(LIBRERAMAN_MACOS_MIN "${CMAKE_OSX_DEPLOYMENT_TARGET}")
    if(NOT LIBRERAMAN_MACOS_MIN)
      set(LIBRERAMAN_MACOS_MIN 12.0)
    endif()
    # (No Input Monitoring / USB usage description is needed: the MCP2200 is a vendor-defined HID device,
    # not a keyboard, and the app is neither sandboxed nor built with the hardened runtime.)
    configure_file(${LR_PKG}/macos/Info.plist.in ${CMAKE_CURRENT_BINARY_DIR}/Info.plist @ONLY)
    set_target_properties(libreraman PROPERTIES MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_BINARY_DIR}/Info.plist)
    # both executables are in Contents/MacOS; macdeployqt puts the frameworks/dylibs in Contents/Frameworks
    set_target_properties(libreraman libreraman-cli libreraman-mcp PROPERTIES INSTALL_RPATH "@executable_path/../Frameworks")
    install(TARGETS libreraman BUNDLE DESTINATION .)
    # the CLI lives inside the bundle, next to the GUI, so that both share the deployed Qt frameworks
    install(TARGETS libreraman-cli libreraman-mcp RUNTIME DESTINATION LibreRaman.app/Contents/MacOS)
    install(FILES ${LR_LICENSE} ${LR_NOTICES} DESTINATION LibreRaman.app/Contents/Resources)
  else()
    install(TARGETS libreraman-cli libreraman-mcp RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  endif()

# ---------------------------------------------------------------- Linux / other Unix
else()
  install(TARGETS libreraman-cli libreraman-mcp RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  if(TARGET libreraman)
    install(TARGETS libreraman RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES ${LR_PKG}/linux/${LIBRERAMAN_APP_ID}.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES ${LR_PKG}/linux/${LIBRERAMAN_APP_ID}.metainfo.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
    set(lr_sizes 16 24 32 48 64 128 256 512)
    set(lr_icon_dir ${CMAKE_CURRENT_BINARY_DIR}/hicolor)
    set(lr_icons "")
    foreach(s ${lr_sizes})
      list(APPEND lr_icons ${lr_icon_dir}/${s}x${s}.png)
    endforeach()
    add_custom_command(OUTPUT ${lr_icons} COMMAND mkicon png ${LR_ICON_PNG} ${lr_icon_dir}
                       DEPENDS mkicon ${LR_ICON_PNG} COMMENT "hicolor icons")
    add_custom_target(libreraman_icons ALL DEPENDS ${lr_icons})
    foreach(s ${lr_sizes})
      install(FILES ${lr_icon_dir}/${s}x${s}.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${s}x${s}/apps
              RENAME ${LIBRERAMAN_APP_ID}.png)
    endforeach()
  endif()
  # udev looks in /usr/lib/udev/rules.d (packages) and /etc/udev/rules.d (local), not under /usr/local.
  set(LIBRERAMAN_UDEV_RULES_DIR "lib/udev/rules.d" CACHE STRING "udev rules directory (relative to the prefix or absolute)")
  install(FILES ${LR_PKG}/linux/70-libreraman.rules DESTINATION ${LIBRERAMAN_UDEV_RULES_DIR})
  install(FILES ${LR_LICENSE} DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/libreraman RENAME copyright)
  install(FILES ${LR_NOTICES} DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/libreraman)
endif()

# ---------------------------------------------------------------- CPack (Linux: .deb)
set(CPACK_PACKAGE_NAME libreraman)
set(CPACK_PACKAGE_VENDOR "LibreRaman contributors")
set(CPACK_PACKAGE_CONTACT "LibreRaman contributors <https://github.com/ohlincha/libreraman/issues>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Open software for Raman spectrometers (Anton Paar Cora 5000)")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/ohlincha/libreraman")
set(CPACK_RESOURCE_FILE_LICENSE ${LR_LICENSE})
set(CPACK_STRIP_FILES ON)
if(UNIX AND NOT APPLE)
  # Built on Ubuntu 24.04 for Ubuntu 24.04+ and Debian 13+: dpkg-shlibdeps derives the library packages
  # (libqt6core6t64, libusb-1.0-0, libhidapi-hidraw0, ...). Plugins aren't linked, so they are listed here.
  # -DLIBRERAMAN_DEB_SHLIBDEPS=OFF (e.g. with a Qt not from the distribution) uses a fixed list instead.
  option(LIBRERAMAN_DEB_SHLIBDEPS "Derive the .deb dependencies with dpkg-shlibdeps" ON)
  set(CPACK_GENERATOR DEB)
  set(CPACK_PACKAGING_INSTALL_PREFIX /usr)
  set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
  set(CPACK_DEBIAN_PACKAGE_SECTION science)
  set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${CPACK_PACKAGE_CONTACT}")
  set(CPACK_PACKAGE_DESCRIPTION
      "LibreRaman talks directly over USB to Raman spectrometers whose vendor software needs an old,\n"
      "unsupported operating system (first: Anton Paar Cora 5000). Measures spectra and time series,\n"
      "calibrates against reference standards, keeps a spectral library. Includes a simulator.")
  string(REPLACE ";" "" CPACK_PACKAGE_DESCRIPTION "${CPACK_PACKAGE_DESCRIPTION}")
  set(lr_plugins "qt6-qpa-plugins, libqt6sql6-sqlite")
  if(LIBRERAMAN_DEB_SHLIBDEPS)
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "${lr_plugins}, libqt6sql6")
  else()
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "${lr_plugins}, libqt6core6t64, libqt6gui6, libqt6widgets6, libqt6network6, "
        "libqt6sql6, libqt6serialport6, libqt6svg6, libusb-1.0-0, libhidapi-hidraw0")
    string(REPLACE ";" "" CPACK_DEBIAN_PACKAGE_DEPENDS "${CPACK_DEBIAN_PACKAGE_DEPENDS}")
  endif()
  set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "qt6-wayland")
  # called libreramen up to 0.3.9: installing this package replaces that one
  set(CPACK_DEBIAN_PACKAGE_CONFLICTS "libreramen")
  set(CPACK_DEBIAN_PACKAGE_REPLACES "libreramen")
  set(CPACK_DEBIAN_PACKAGE_PROVIDES "libreramen")
  set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA ${LR_PKG}/linux/postinst ${LR_PKG}/linux/postrm)
  set(CPACK_DEBIAN_PACKAGE_CONTROL_STRICT_PERMISSION ON)
  set(CPACK_INSTALL_DEFAULT_DIRECTORY_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE
      WORLD_READ WORLD_EXECUTE)
endif()
include(CPack)
