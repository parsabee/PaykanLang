# BisonFlexSetup.cmake - Downloads and builds Bison and Flex from source

include(ExternalProject)

# ── Bison ────────────────────────────────────────────────────────────────────
if(NOT EXISTS ${BISON_INSTALL_DIR}/bin/bison)
    message(STATUS "Building Bison ${BISON_VERSION} from source...")
    ExternalProject_Add(bison_ext
        URL                        "https://ftp.gnu.org/gnu/bison/bison-${BISON_VERSION}.tar.gz"
        PREFIX                     ${THIRD_PARTY_DIR}/bison_build
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        CONFIGURE_COMMAND          <SOURCE_DIR>/configure --prefix=${BISON_INSTALL_DIR}
        BUILD_COMMAND              make -j4
        INSTALL_COMMAND            make install
        LOG_DOWNLOAD ON
        LOG_BUILD    ON
        LOG_INSTALL  ON
    )
else()
    message(STATUS "Found Bison in ${BISON_INSTALL_DIR}")
    add_custom_target(bison_ext)
endif()
set(BISON_EXECUTABLE ${BISON_INSTALL_DIR}/bin/bison)

# ── Flex ─────────────────────────────────────────────────────────────────────
if(NOT EXISTS ${FLEX_INSTALL_DIR}/bin/flex)
    message(STATUS "Building Flex ${FLEX_VERSION} from source...")
    ExternalProject_Add(flex_ext
        URL                        "https://github.com/westes/flex/releases/download/v${FLEX_VERSION}/flex-${FLEX_VERSION}.tar.gz"
        PREFIX                     ${THIRD_PARTY_DIR}/flex_build
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        CONFIGURE_COMMAND          <SOURCE_DIR>/configure --prefix=${FLEX_INSTALL_DIR}
        BUILD_COMMAND              make -j4
        INSTALL_COMMAND            make install
        LOG_DOWNLOAD ON
        LOG_BUILD    ON
        LOG_INSTALL  ON
    )
else()
    message(STATUS "Found Flex in ${FLEX_INSTALL_DIR}")
    add_custom_target(flex_ext)
endif()
set(FLEX_EXECUTABLE ${FLEX_INSTALL_DIR}/bin/flex)
