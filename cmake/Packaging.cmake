# CPack: the Debian/Ubuntu package (`cpack -G DEB` in a configured build tree).
#
#   cmake -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build
#   (cd build && cpack -G DEB)        # -> paykanlang_<version>_<arch>.deb
#   sudo apt install ./build/paykanlang_<version>_<arch>.deb
#
# The package holds what `cmake --install` installs, under /usr: bin/paykan,
# the runtime archive and its headers (include/paykan), the C plugin
# interface (plugin_api.h, plugin_api_version.h), the find_package(Paykan)
# CMake package and the empty system plugin directory
# lib/paykan/plugins/<version>, plus the license as
# share/doc/paykanlang/copyright.  The frontend test support
# (PaykanTestSupport.cmake, its own install component) is left out.
# `paykan` finds the runtime and the plugin directory relative to its own
# executable, so /usr/bin/paykan uses /usr/lib and /usr/include.
#
# The version is PAYKAN_VERSION, with a pre-release label in Debian's form
# ("1.2.3-rc1" becomes "1.2.3~rc1", which sorts before "1.2.3").  The
# package depends on the shared libraries `paykan` links (computed by
# dpkg-shlibdeps) and on a C compiler that provides `cc` (gcc or clang, each of
# which registers the `cc` alternative) with the C library headers: the c
# backend runs `cc` to compile and link every program.
#
# Included last from the top-level CMakeLists.txt, after every install() rule.

# The license, at the path Debian policy expects.  EXCLUDE_FROM_ALL: only the
# package installs it; a plain `cmake --install` does not.
install(FILES ${PROJECT_SOURCE_DIR}/LICENSE
    DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/paykanlang
    RENAME copyright
    COMPONENT DebianDocs
    EXCLUDE_FROM_ALL
)

set(CPACK_PACKAGE_NAME "paykanlang")
set(CPACK_PACKAGE_VENDOR "Parsa Bagheri")
set(CPACK_PACKAGE_CONTACT "Parsa Bagheri <35123665+parsabee@users.noreply.github.com>")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/parsabee/PaykanLang")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "High-performance language for applications on heterogeneous machines")
set(CPACK_PACKAGE_DESCRIPTION
    "PaykanLang (.pkn) is a statically-typed, object-oriented language with
automatic reference counting, a module system and generics.  This package
holds the paykan compiler (the recursive-descent frontend and the C backend,
which compiles programs with the system C compiler), its runtime, and the
headers and CMake package for building frontend and backend plugins.")
set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGE_VERSION_MAJOR ${PAYKAN_VERSION_MAJOR})
set(CPACK_PACKAGE_VERSION_MINOR ${PAYKAN_VERSION_MINOR})
set(CPACK_PACKAGE_VERSION_PATCH ${PAYKAN_VERSION_PATCH})
set(CPACK_PACKAGE_VERSION ${PAYKAN_VERSION})
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")

# What goes in: the default install component (everything but the frontend
# test support) and the license.
set(CPACK_INSTALL_CMAKE_PROJECTS
    "${CMAKE_BINARY_DIR};${PROJECT_NAME};Unspecified;/"
    "${CMAKE_BINARY_DIR};${PROJECT_NAME};DebianDocs;/")

# -- DEB ----------------------------------------------------------------------
string(REPLACE "-" "~" CPACK_DEBIAN_PACKAGE_VERSION "${PAYKAN_VERSION}")
find_program(PAYKAN_DPKG dpkg)
mark_as_advanced(PAYKAN_DPKG)
if(PAYKAN_DPKG)
    execute_process(COMMAND ${PAYKAN_DPKG} --print-architecture
        OUTPUT_VARIABLE CPACK_DEBIAN_PACKAGE_ARCHITECTURE
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
endif()
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)   # paykanlang_<version>_<arch>.deb
set(CPACK_DEBIAN_PACKAGE_SECTION "devel")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_DEPENDS "gcc | clang, libc6-dev | libc-dev")

include(CPack)
