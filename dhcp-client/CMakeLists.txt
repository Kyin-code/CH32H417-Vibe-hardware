cmake_minimum_required(VERSION 3.16)
project(dhcp_client_host C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

enable_testing()

add_library(dhcp_client STATIC
    src/dhcp_packet.c
    src/dhcp_client.c
    src/dhcp_lease.c
)

target_include_directories(dhcp_client PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(dhcp_client PRIVATE
    -Wall -Wextra -Werror -pedantic
)

# Host port used by the tests (and by any desktop simulation).
add_library(dhcp_port_host STATIC
    ports/host/dhcp_port_host.c
)

target_include_directories(dhcp_port_host PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/ports/host
)

target_link_libraries(dhcp_port_host PUBLIC dhcp_client)

target_compile_options(dhcp_port_host PRIVATE
    -Wall -Wextra -Werror -pedantic
)

# --- tests -----------------------------------------------------------------
option(DHCP_TESTS "Build the unit tests" ON)
option(DHCP_SANITIZE "Build tests with -fsanitize=address,undefined" OFF)

if(DHCP_TESTS)
    add_executable(dhcp_tests
        tests/main.c
        tests/test_support.c
        tests/test_packet.c
        tests/test_client.c
        tests/test_policy.c
        examples/lease_apply_policy.c
    )

    target_include_directories(dhcp_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
        ${CMAKE_CURRENT_SOURCE_DIR}/examples
    )

    target_link_libraries(dhcp_tests PRIVATE dhcp_client dhcp_port_host)

    target_compile_options(dhcp_tests PRIVATE
        -Wall -Wextra -Werror -pedantic -fstack-protector-strong
    )

    if(DHCP_SANITIZE)
        target_compile_options(dhcp_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(dhcp_tests PRIVATE -fsanitize=address,undefined)
    endif()

    add_test(NAME dhcp_unit_tests COMMAND dhcp_tests)
endif()

# --- CH32H417 port: header selection --------------------------------------
# Prefer the real vendor header from the nanoCH32H417 tree; fall back to the
# checked-in stub so the checks always exist.
set(_wch_netlib "${CMAKE_CURRENT_SOURCE_DIR}/../nanoCH32H417-main/doc/EVT/EXAM/ETH/NetLib")
set(_wch_common "${CMAKE_CURRENT_SOURCE_DIR}/../nanoCH32H417-main/doc/EVT/EXAM/ETH/DHCP/Common")
set(_wch_inc_dirs "")

if(EXISTS "${_wch_netlib}/wchnet.h" AND EXISTS "${_wch_common}/net_config.h")
    set(_wch_inc_dirs "${_wch_netlib}" "${_wch_common}")
    set(_wch_hdr_kind "vendor header (${_wch_netlib})")
else()
    set(_wch_inc_dirs "${CMAKE_CURRENT_SOURCE_DIR}/ports/ch32h417/_selfcheck")
    set(_wch_hdr_kind "stub header (vendor tree not found)")
endif()
message(STATUS "WCHNET checks use: ${_wch_hdr_kind}")

set(_wch_incs "")
foreach(_d IN LISTS _wch_inc_dirs)
    list(APPEND _wch_incs "-I${_d}")
endforeach()

# --- type check ------------------------------------------------------------
option(DHCP_WCHNET_SELFCHECK "Type-check the WCHNET port against the vendor headers" ON)

if(DHCP_WCHNET_SELFCHECK)
    foreach(_mode 1 0)
        add_test(NAME wchnet_selfcheck_udp_${_mode}
            COMMAND ${CMAKE_C_COMPILER}
                -fsyntax-only -std=c99
                -Wall -Wextra -Werror -pedantic
                -Wshadow -Wconversion -Wsign-conversion -Wcast-qual
                -Wstrict-prototypes
                -DDHCP_WCHNET_USE_UDP_SOCKET=${_mode}
                -I${CMAKE_CURRENT_SOURCE_DIR}/include
                ${_wch_incs}
                ${CMAKE_CURRENT_SOURCE_DIR}/ports/ch32h417/dhcp_port_wchnet.c)
    endforeach()
endif()

# --- execution against a fake WCHNET stack ---------------------------------
option(DHCP_WCHNET_SIM "Run the WCHNET port against the fake stack" ON)

if(DHCP_WCHNET_SIM)
    foreach(_mode 1 0)
        add_executable(dhcp_tests_wchnet_${_mode}
            tests/test_wchnet_port.c
            tests/test_support.c
            ports/ch32h417/dhcp_port_wchnet.c
            ports/ch32h417/sim/wchnet_sim.c
        )
        target_include_directories(dhcp_tests_wchnet_${_mode} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/tests
            ${CMAKE_CURRENT_SOURCE_DIR}/ports/ch32h417
            ${CMAKE_CURRENT_SOURCE_DIR}/ports/ch32h417/sim
            ${_wch_inc_dirs}
        )
        target_compile_definitions(dhcp_tests_wchnet_${_mode} PRIVATE
            DHCP_WCHNET_USE_UDP_SOCKET=${_mode}
        )
        target_link_libraries(dhcp_tests_wchnet_${_mode} PRIVATE dhcp_client dhcp_port_host)
        target_compile_options(dhcp_tests_wchnet_${_mode} PRIVATE
            -Wall -Wextra -Werror -pedantic -fstack-protector-strong
        )
        add_test(NAME wchnet_port_udp_${_mode} COMMAND dhcp_tests_wchnet_${_mode})
    endforeach()
endif()

# --- firmware upload (CRC-checked, no filesystem) ---------------------------
add_executable(fw_upgrade_tests
    tests/test_fw_upgrade.c
    target/fw_upgrade.c
)
target_include_directories(fw_upgrade_tests PRIVATE target)
target_compile_options(fw_upgrade_tests PRIVATE
    -Wall -Wextra -Werror -pedantic -fstack-protector-strong
)
add_test(NAME fw_upgrade COMMAND fw_upgrade_tests)
