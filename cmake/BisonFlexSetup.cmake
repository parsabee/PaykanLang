# BisonFlexSetup.cmake
# ----------------------------------------------------------------------------
# Builds Bison and Flex from source via ExternalProject and installs them
# into ${BISON_INSTALL_DIR} / ${FLEX_INSTALL_DIR}.  Skips the build if the
# binaries already exist from a previous configure.
#
# Provides:
#   BISON_EXECUTABLE  -- path to the built bison binary
#   FLEX_EXECUTABLE   -- path to the built flex binary
#   bison_ext         -- target that other targets can depend on
#   flex_ext          -- target that other targets can depend on
# ----------------------------------------------------------------------------

include(ExternalProject)

# Use all available cores for building autotools projects.
include(ProcessorCount)
ProcessorCount(NPROC)
if(NPROC EQUAL 0)
    set(NPROC 4)
endif()

# -- Bison --------------------------------------------------------------------
if(NOT EXISTS ${BISON_INSTALL_DIR}/bin/bison)
    message(STATUS "Building Bison ${BISON_VERSION} from source...")
    ExternalProject_Add(bison_ext
        # Several sources: CMake tries each in turn.  ftp.gnu.org throttles
        # CI runners and has failed the sanitizer jobs outright.
        URL                        "https://ftpmirror.gnu.org/gnu/bison/bison-${BISON_VERSION}.tar.gz"
                                   "https://mirrors.kernel.org/gnu/bison/bison-${BISON_VERSION}.tar.gz"
                                   "https://ftp.gnu.org/gnu/bison/bison-${BISON_VERSION}.tar.gz"
        URL_HASH                   SHA256=d5d184d421aee15603939973a6b0f372f908edfb24c5bc740697497021ad9458
        PREFIX                     ${THIRD_PARTY_DIR}/bison_build
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        CONFIGURE_COMMAND          <SOURCE_DIR>/configure --prefix=${BISON_INSTALL_DIR}
        BUILD_COMMAND              make -j${NPROC}
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

# -- Flex ---------------------------------------------------------------------
if(NOT EXISTS ${FLEX_INSTALL_DIR}/bin/flex)
    message(STATUS "Building Flex ${FLEX_VERSION} from source...")
    ExternalProject_Add(flex_ext
        URL                        "https://github.com/westes/flex/releases/download/v${FLEX_VERSION}/flex-${FLEX_VERSION}.tar.gz"
        URL_HASH                   SHA256=e87aae032bf07c26f85ac0ed3250998c37621d95f8bd748b31f15b33c45ee995
        PREFIX                     ${THIRD_PARTY_DIR}/flex_build
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        CONFIGURE_COMMAND          <SOURCE_DIR>/configure --prefix=${FLEX_INSTALL_DIR}
        BUILD_COMMAND              make -j${NPROC}
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
