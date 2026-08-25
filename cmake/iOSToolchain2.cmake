# Configurable variables.
#
# IOS_PLATFORM [iphoneos | iphonesimulator]     (required)
# IOS_ARCH     [arm64 | x86_64]                 (optional, default arm64)
# IOS_DEPLOYMENT_TARGET                         (optional, default 13.0)
#
# Same as cmake/iOSToolchain.cmake, except that IOS_ARCH is honoured
# independently from IOS_PLATFORM so that an arm64 iPhone Simulator
# slice can be produced on Apple Silicon hosts.

# Targeting system
set(CMAKE_SYSTEM_NAME Darwin)
set(CMAKE_C_COMPILER_ID Clang)
set(IOS TRUE)

if(NOT IOS_DEPLOYMENT_TARGET)
    if(NOT "$ENV{IOS_DEPLOYMENT_TARGET}" STREQUAL "")
        set(IOS_DEPLOYMENT_TARGET "$ENV{IOS_DEPLOYMENT_TARGET}")
    else()
        set(IOS_DEPLOYMENT_TARGET "13.0")
    endif()
endif()
set(ENV{IOS_DEPLOYMENT_TARGET} ${IOS_DEPLOYMENT_TARGET})

set(CMAKE_MODULE_PATH ${CMAKE_CURRENT_LIST_DIR})
include(CheckBuildOptions)

# Check iOS Platform target variable invalid.
check_build_option(IOS_PLATFORM)
if(${IOS_PLATFORM} STREQUAL "NOTFOUND")
    message(FATAL_ERROR "Variable IOS_PLATFORM not set")
endif()

check_build_option(IOS_ARCH)
if(${IOS_ARCH} STREQUAL "NOTFOUND")
    set(IOS_ARCH "arm64")
    set(ENV{IOS_ARCH} ${IOS_ARCH})
endif()

# Update CMAKE_TOOLCHAIN_FILE to absolute path
get_filename_component(CMAKE_TOOLCHAIN_FILE
        "${CMAKE_TOOLCHAIN_FILE}"
        ABSOLUTE
        BASE_DIR "${CMAKE_CURRENT_BINARY_DIR}")

# CMAKE_SYSTEM_PROCESSOR
set(CMAKE_SYSTEM_PROCESSOR  ${IOS_ARCH})
# CMAKE_OSX_ARCHITECTURES
set(CMAKE_OSX_ARCHITECTURES ${IOS_ARCH})

# CMAKE_OSX_SYSROOT
execute_process(
    COMMAND xcodebuild -version -sdk ${IOS_PLATFORM} Path
    OUTPUT_VARIABLE IOS_SYSROOT
    ERROR_QUIET
    OUTPUT_STRIP_TRAILING_WHITESPACE)

set(CMAKE_OSX_SYSROOT ${IOS_SYSROOT})

# CMAKE_SYSTEM_VERSION
execute_process(
    COMMAND xcodebuild -sdk ${CMAKE_OSX_SYSROOT} -version SDKVersion
    OUTPUT_VARIABLE IOS_SDK_VERSION
    ERROR_QUIET
    OUTPUT_STRIP_TRAILING_WHITESPACE)

set(CMAKE_SYSTEM_VERSION ${IOS_SDK_VERSION})

# CMAKE_<lang>_FLAGS
set(IOS_BITCODE_FLAG "")

if(${IOS_PLATFORM} STREQUAL "iphoneos")
    set(IOS_DEPLOYMENT_TARGET_FLAG "-miphoneos-version-min=")
else()
    set(IOS_DEPLOYMENT_TARGET_FLAG "-mios-simulator-version-min=")
endif()

if(${IOS_ARCH} STREQUAL "arm64")
    set(IOS_ARCH_FLAGS "-arch arm64 -D__arm64__")
elseif(${IOS_ARCH} STREQUAL "x86_64")
    set(IOS_ARCH_FLAGS "-arch x86_64 -D__x86_64__")
else()
    message(FATAL_ERROR "Unsupported IOS_ARCH: ${IOS_ARCH}")
endif()

string(CONCAT _CMAKE_C_FLAGS
    "${CAKE_C_FLAGS_INIT}"
    "${IOS_DEPLOYMENT_TARGET_FLAG}${IOS_DEPLOYMENT_TARGET} "
    "${IOS_ARCH_FLAGS} "
    "${IOS_BITCODE_FLAG} "
    "-fobjc-abi-version=2 "
    "-isysroot ${CMAKE_OSX_SYSROOT}")

set(CMAKE_C_FLAGS ${_CMAKE_C_FLAGS})
set(CMAKE_CXX_FLAGS ${_CMAKE_C_FLAGS})
