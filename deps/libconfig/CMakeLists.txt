project(libconfig)

include(CarrierDefaults)
include(ExternalProject)

if(WIN32)
    set_win_build_options(WIN_BUILD_OPTIONS "Static")

    set(BUILD_CMD "msbuild")
    set(BUILD_ARGS ${WIN_BUILD_OPTIONS}
        "libconfig_vs2017.sln")

    set(INSTALL_CMD "echo")
    set(INSTALL_ARGS "Done")
else()
    # Use CMake build instead of autotools
    set(BUILD_CMD "make")
    set(INSTALL_CMD "make")
    set(INSTALL_ARGS "install")
endif()

ExternalProject_Add(
    libconfig

    PREFIX ${CARRIER_DEPS_BUILD_PREFIX}
    URL "https://github.com/hyperrealm/libconfig/archive/v1.7.2.tar.gz"
    URL_HASH SHA256=f67ac44099916ae260a6c9e290a90809e7d782d96cdd462cac656ebc5b685726
    DOWNLOAD_NAME "libconfig-1.7.2.tar.gz"
    DOWNLOAD_DIR ${CARRIER_DEPS_TARBALL_DIR}
    DOWNLOAD_NO_PROGRESS 1


    CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=${CARRIER_INT_DIST_DIR}
        -DBUILD_SHARED_LIBS=OFF
        -DBUILD_EXAMPLES=OFF
        -DBUILD_TESTS=OFF
        ${CMAKE_ARGS_INIT}
    BUILD_COMMAND ${BUILD_CMD} libconfig ${BUILD_ARGS}
    INSTALL_COMMAND ${CMAKE_COMMAND} -E copy_if_different
        <SOURCE_DIR>/lib/libconfig.h ${CARRIER_INT_DIST_DIR}/include/libconfig.h
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        <BINARY_DIR>/out/liblibconfig.a ${CARRIER_INT_DIST_DIR}/lib/libconfig.a
)
