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
