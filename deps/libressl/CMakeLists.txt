project(libressl)

include(CarrierDefaults)
include(ExternalProject)
include(ExternalCMakeArgs)

# LibreSSL 4.3.4 (stable branch, 2026). Hash checked against
# https://ftp.openbsd.org/pub/OpenBSD/LibreSSL/SHA256 on 2026-10-24.
# 2.9.2 (2019) could not complete a TLS handshake with an ECDSA server
# certificate (CURLE 35), which forced the RSA pin on the Express hosts.
# The old 2.9.2 cmake_minimum_required patch is not needed: 4.x asks for
# CMake 3.16.4 itself.
set(LIBRESSL_VERSION 4.3.4)

ExternalProject_Add(
    libressl

    PREFIX ${CARRIER_DEPS_BUILD_PREFIX}
    URL "https://ftp.openbsd.org/pub/OpenBSD/LibreSSL/libressl-${LIBRESSL_VERSION}.tar.gz"
    URL_HASH SHA256=57c8b2f8c6f9505587abaeddab003ea243d615edb7ed71957968121fc6316bd6
    DOWNLOAD_NAME "libressl-${LIBRESSL_VERSION}.tar.gz"
    DOWNLOAD_DIR ${CARRIER_DEPS_TARBALL_DIR}
    DOWNLOAD_NO_PROGRESS 1

    CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=${CARRIER_INT_DIST_DIR}
        -DLIBRESSL_SKIP_INSTALL=OFF
        -DLIBRESSL_APPS=OFF
        -DLIBRESSL_TESTS=OFF
        -DBUILD_SHARED_LIBS=OFF
        -DENABLE_ASM=OFF
        -DENABLE_EXTRATESTS=OFF
        -DENABLE_NC=OFF
        ${CMAKE_ARGS_INIT}
)

if(ENABLE_STATIC)
    export_static_library(ssl)
    export_static_library(tls)
    export_static_library(crypto)
endif()
