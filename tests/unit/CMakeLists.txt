project(carrier-unit-tests C)
include(CarrierDefaults)

set(SRC identity_test.c ../../src/carrier/xeddsa.c)

if(${CMAKE_SYSTEM_NAME} STREQUAL "Linux")
    set(SYSTEM_LIBS pthread m)
endif()

include_directories(
    ../../src/carrier
    ${CARRIER_INT_DIST_DIR}/include)

link_directories(
    ${CARRIER_INT_DIST_DIR}/lib
    ${CMAKE_CURRENT_BINARY_DIR}/../../src/carrier)

if(ENABLE_SHARED)
    add_definitions(-DCRYSTAL_DYNAMIC)
else()
    add_definitions(-DCRYSTAL_STATIC)
endif()

set(LIBS carrier crystal sodium)

add_executable(identity_test ${SRC})
target_link_libraries(identity_test ${LIBS} ${SYSTEM_LIBS})

# friendreq_test: the requester's avatar on a friend request. The OLD 3-field
# schema (packet_v1.fbs) is generated in its own namespace so the test can run
# the shipped reader/verifier/builder against the new packet.c.
add_custom_command(
    OUTPUT packet_v1_generated.h
    COMMAND ${CARRIER_HOST_TOOLS_DIR}/bin/flatcc${CMAKE_EXECUTABLE_SUFFIX}
        --outfile packet_v1_generated.h
        -a "${CMAKE_CURRENT_LIST_DIR}/packet_v1.fbs"
    DEPENDS "${CMAKE_CURRENT_LIST_DIR}/packet_v1.fbs" flatcc-parser)

# Linked against the STATIC libcarrier: the test drives internal symbols
# (packet_*, carrier_notify_friend_request) that the dylib does not export.
add_executable(friendreq_test friendreq_test.c packet_v1_generated.h)
target_include_directories(friendreq_test PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}/../../src/carrier
    ${CMAKE_CURRENT_LIST_DIR}/../../src/carrier/dht)
target_compile_definitions(friendreq_test PRIVATE CRYSTAL_STATIC)
target_link_libraries(friendreq_test
    ${CMAKE_CURRENT_BINARY_DIR}/../../src/carrier/libcarrier.a
    cjson libcurl crystal toxcore flatccrt sodium ${SYSTEM_LIBS})
add_dependencies(friendreq_test carrier0)
