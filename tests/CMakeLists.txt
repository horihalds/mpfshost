find_package(Catch2 2 REQUIRED)

add_executable(mpfshost_tests
    main.cpp
    access_policy_test.cpp
    address_lookup_test.cpp
    cli_options_test.cpp
    connect_diagnostics_test.cpp
    debug_controller_test.cpp
    debug_test.cpp
    display_test.cpp
    encoded_size_test.cpp
    smoke_test.cpp
    frame_test.cpp
    frame_fuzz_test.cpp
    handle_cache_test.cpp
    handles_test.cpp
    hex_test.cpp
    key_map_test.cpp
    keyboard_state_test.cpp
    log_test.cpp
    mcp_protocol_test.cpp
    mcp_tools_test.cpp
    message_test.cpp
    memory_test.cpp
    maps_test.cpp
    module_tables_test.cpp
    network_test.cpp
    process_filter_test.cpp
    scanner_test.cpp
    server_test.cpp
    services_test.cpp
    scatter_test.cpp
    symbol_expression_test.cpp
    thread_test.cpp
    unloaded_modules_test.cpp
    users_test.cpp
    heaps_test.cpp
    kernel_drivers_test.cpp
    kernel_devices_test.cpp
    kernel_objects_test.cpp
    pool_pte_filters.cpp
    pool_pte_pfn_roundtrip.cpp
    pool_pte_pfn_session_test.cpp
    qmp_client_test.cpp
    rsp_client_test.cpp
    ipc_end_to_end_test.cpp
    physical_test.cpp
    vfs_test.cpp
    disasm_test.cpp
)
mpfshost_set_common_flags(mpfshost_tests)

# Only unprivileged code: never link mpfshost_host here.
target_link_libraries(mpfshost_tests PRIVATE
    mpfshost_server
    mpfshost_cli
    Catch2::Catch2
)

target_compile_definitions(mpfshost_tests PRIVATE
    MPFSHOST_TEST_TMP_DIR="${PROJECT_SOURCE_DIR}/tmp"
)

include(Catch)
catch_discover_tests(mpfshost_tests)
