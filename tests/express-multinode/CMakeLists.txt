project(carrier-express-multinode-test C)

include(CarrierDefaults)
include(CheckIncludeFile)

check_include_file(unistd.h HAVE_UNISTD_H)
if(HAVE_UNISTD_H)
    add_definitions(-DHAVE_UNISTD_H=1)
endif()
check_include_file(getopt.h HAVE_GETOPT_H)
if(HAVE_GETOPT_H)
    add_definitions(-DHAVE_GETOPT_H=1)
endif()
check_include_file(sys/time.h HAVE_SYS_TIME_H)
if(HAVE_SYS_TIME_H)
    add_definitions(-DHAVE_SYS_TIME_H=1)
endif()

# The test calls connector internals (express_enqueue_post_request,
# express_enqueue_pull_messages, packet_*), which the shared library hides
# (-fvisibility=hidden), so it links the static libcarrier.a and its deps.
set(SRC express_multinode_test.c)

add_definitions(-DCARRIER_STATIC -DCRYSTAL_STATIC)

include_directories(
    ../../src/carrier
    ../../src/carrier/dht
    ${CMAKE_CURRENT_BINARY_DIR}/../../src/carrier
    ${CARRIER_INT_DIST_DIR}/include)

link_directories(
    ${CARRIER_INT_DIST_DIR}/lib)

# Full paths: the bare names (curl, toxcore, ...) are ExternalProject targets
# here, which CMake refuses to link.
set(DEP_LIB_DIR ${CARRIER_INT_DIST_DIR}/lib)
set(LIBS
    ${CMAKE_CURRENT_BINARY_DIR}/../../src/carrier/${CMAKE_STATIC_LIBRARY_PREFIX}carrier${CMAKE_STATIC_LIBRARY_SUFFIX}
    ${DEP_LIB_DIR}/${CMAKE_STATIC_LIBRARY_PREFIX}crystal${CMAKE_STATIC_LIBRARY_SUFFIX}
    ${DEP_LIB_DIR}/${CMAKE_STATIC_LIBRARY_PREFIX}cjson${CMAKE_STATIC_LIBRARY_SUFFIX}
    ${DEP_LIB_DIR}/${CMAKE_STATIC_LIBRARY_PREFIX}curl${CMAKE_STATIC_LIBRARY_SUFFIX}
    ${DEP_LIB_DIR}/${CMAKE_STATIC_LIBRARY_PREFIX}ssl${CMAKE_STATIC_LIBRARY_SUFFIX}
    ${DEP_LIB_DIR}/${CMAKE_STATIC_LIBRARY_PREFIX}crypto${CMAKE_STATIC_LIBRARY_SUFFIX}
    ${DEP_LIB_DIR}/${CMAKE_STATIC_LIBRARY_PREFIX}z${CMAKE_STATIC_LIBRARY_SUFFIX}
    ${DEP_LIB_DIR}/${CMAKE_STATIC_LIBRARY_PREFIX}toxcore${CMAKE_STATIC_LIBRARY_SUFFIX}
    ${DEP_LIB_DIR}/${CMAKE_STATIC_LIBRARY_PREFIX}flatccrt${CMAKE_STATIC_LIBRARY_SUFFIX}
    ${DEP_LIB_DIR}/${CMAKE_STATIC_LIBRARY_PREFIX}sodium${CMAKE_STATIC_LIBRARY_SUFFIX}
    pthread)

if(${CMAKE_SYSTEM_NAME} STREQUAL "Linux")
    set(LIBS ${LIBS} m)
endif()

add_executable(express_multinode_test ${SRC})
add_dependencies(express_multinode_test carrier-static)
target_link_libraries(express_multinode_test ${LIBS})
