cmake_minimum_required(VERSION 3.25)
project(mpfshost VERSION 0.1.0 LANGUAGES C CXX)

find_package(Threads REQUIRED)

include(FetchContent)
include(GNUInstallDirs)

# The MCP mode speaks JSON-RPC, so it needs a JSON implementation. This is the only third-party dependency: it is
# header-only, pinned by release tag and checksum, and marked SYSTEM so its own headers do not trip -Werror.
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install OFF CACHE BOOL "" FORCE)
FetchContent_Declare(nlohmann_json
    SYSTEM
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
    DOWNLOAD_EXTRACT_TIMESTAMP ON
)
FetchContent_MakeAvailable(nlohmann_json)

option(MPFSHOST_BUILD_EXAMPLES "Build the client API examples" ON)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

function(mpfshost_set_common_flags target)
    target_compile_features(${target} PRIVATE cxx_std_23)
    target_compile_options(${target} PRIVATE
        -Wall -Wextra
        -Werror
        -fno-strict-aliasing
    )
endfunction()

#Unprivileged : must never depend on vmm, leechcore or root - only state.
add_library(mpfshost_core STATIC
    src/core/version.cpp
    src/core/log/log.cpp
    src/core/ipc/frame.cpp
    src/core/ipc/message_core.cpp
    src/core/ipc/message_memory.cpp
    src/core/ipc/message_process.cpp
    src/core/ipc/message_maps.cpp
    src/core/ipc/message_system.cpp
    src/core/ipc/message_kernel.cpp
    src/core/ipc/message_vfs.cpp
    src/core/target/scanner.cpp
    src/core/server/session.cpp
    src/core/server/session_memory.cpp
    src/core/server/session_process.cpp
    src/core/server/session_maps.cpp
    src/core/server/session_system.cpp
    src/core/server/session_kernel.cpp
    src/core/server/session_vfs.cpp
    src/core/server/session_scatter.cpp
    src/core/server/access_policy.cpp
    src/core/server/handle_cache.cpp
    src/core/server/memory_provider.cpp
    src/core/server/process_provider.cpp
    src/core/server/module_provider.cpp
    src/core/server/memory_map_provider.cpp
    src/core/server/thread_provider.cpp
    src/core/server/handle_provider.cpp
    src/core/server/unloaded_module_provider.cpp
    src/core/server/service_provider.cpp
    src/core/server/user_provider.cpp
    src/core/server/network_provider.cpp
    src/core/server/kernel_driver_provider.cpp
    src/core/server/kernel_device_provider.cpp
    src/core/server/kernel_object_provider.cpp
    src/core/server/pte_provider.cpp
    src/core/server/pool_provider.cpp
    src/core/server/vfs_provider.cpp
    src/core/server/vm_provider.cpp
    src/core/server/physical_memory_provider.cpp
    src/core/server/request_deadline.cpp
    src/core/server/server.cpp
    src/core/client/cli_options.cpp
    src/core/client/client_kernel.cpp
    src/core/client/client_maps.cpp
    src/core/client/client_memory.cpp
    src/core/client/client_process.cpp
    src/core/client/client_system.cpp
    src/core/client/client_transport.cpp
    src/core/client/client_vfs.cpp
    src/core/client/connect_diagnostics.cpp
    src/core/mcp/mcp_server.cpp
    src/core/mcp/mcp_tools.cpp
    src/core/util/display.cpp
    src/core/util/hex.cpp
    src/core/util/number.cpp
    src/core/util/identity.cpp
    src/core/util/page_range.cpp
    src/core/util/text.cpp
)
mpfshost_set_common_flags(mpfshost_core)
target_compile_definitions(mpfshost_core PRIVATE MPFSHOST_VERSION="${PROJECT_VERSION}")
target_include_directories(mpfshost_core PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(mpfshost_core PUBLIC Threads::Threads PRIVATE nlohmann_json::nlohmann_json)

add_library(mpfshost_host STATIC
    src/host/device_string.cpp
    src/host/vmm_handle_factory.cpp
    src/host/filesystem_target_source.cpp
    src/host/vmm_handle.cpp
    src/host/vmm_config.cpp
    src/host/vmm_handles.cpp
    src/host/vmm_heaps.cpp
    src/host/vmm_kernel_devices.cpp
    src/host/vmm_kernel_drivers.cpp
    src/host/vmm_kernel_objects.cpp
    src/host/vmm_memory.cpp
    src/host/vmm_maps.cpp
    src/host/vmm_module_tables.cpp
    src/host/vmm_modules.cpp
    src/host/vmm_pdb.cpp
    src/host/vmm_pfn.cpp
    src/host/vmm_pool.cpp
    src/host/vmm_pte.cpp
    src/host/vmm_processes.cpp
    src/host/vmm_scatter.cpp
    src/host/vmm_threads.cpp
    src/host/vmm_unloaded_modules.cpp
    src/host/vmm_services.cpp
    src/host/vmm_users.cpp
    src/host/vmm_network.cpp
    src/host/vmm_vfs.cpp
    src/host/vmm_physical.cpp
    src/host/vmm_search.cpp
    src/host/vmm_vm.cpp
)
mpfshost_set_common_flags(mpfshost_host)
target_compile_definitions(mpfshost_host PUBLIC LINUX)
target_include_directories(mpfshost_host SYSTEM PUBLIC "${CMAKE_SOURCE_DIR}/MemProcFS")
#The vendored libraries carry no SONAME, so `- l :` keeps their bare names in DT_NEEDED; the installed
#binary resolves them through its `$ORIGIN` RUNPATH while the build tree points at MemProcFS /.
target_link_directories(mpfshost_host PUBLIC "${CMAKE_SOURCE_DIR}/MemProcFS")
target_link_libraries(mpfshost_host PUBLIC
    "-l:vmm.so"
    "-l:leechcore.so"
    mpfshost_core
)

add_executable(${PROJECT_NAME}
    src/main.cpp
    src/cli/options.cpp
    src/cli/render.cpp
    src/cli/run_client.cpp
    src/cli/usage.cpp
)
mpfshost_set_common_flags(${PROJECT_NAME})
target_precompile_headers(${PROJECT_NAME} PRIVATE "${PROJECT_SOURCE_DIR}/src/pch.hpp")
target_link_libraries(${PROJECT_NAME} PRIVATE mpfshost_host)

set_target_properties(${PROJECT_NAME} PROPERTIES
    BUILD_RPATH "${CMAKE_SOURCE_DIR}/MemProcFS"
    INSTALL_RPATH "$ORIGIN"
)

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
    ${CMAKE_SOURCE_DIR}/MemProcFS/info.db
    ${CMAKE_CURRENT_BINARY_DIR}/info.db
    COMMAND ${CMAKE_COMMAND} -E copy
    ${CMAKE_SOURCE_DIR}/MemProcFS/*.so
    ${CMAKE_CURRENT_BINARY_DIR}/
    # MemProcFS resolves both directories relative to the executable and reports a missing plugins directory as a
    # load failure, while an absent Symbols directory leaves it nowhere to cache the PDBs it downloads.
    COMMAND ${CMAKE_COMMAND} -E make_directory
    ${CMAKE_CURRENT_BINARY_DIR}/Symbols
    ${CMAKE_CURRENT_BINARY_DIR}/plugins
)

if(MPFSHOST_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# Optional Windows guest probe, cross-compiled with mingw. It is never linked into the host or the tests.
find_program(MINGW_CXX x86_64-w64-mingw32-g++)
if(MINGW_CXX)
    set(GUEST_PROBE_OUTPUT "${CMAKE_BINARY_DIR}/guest/guest-probe.exe")
    add_custom_command(
        OUTPUT "${GUEST_PROBE_OUTPUT}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/guest"
        COMMAND "${MINGW_CXX}" -O1 -g -static -Wall -Wextra
                -o "${GUEST_PROBE_OUTPUT}" "${CMAKE_SOURCE_DIR}/tools/guest-probe/main.c"
        DEPENDS "${CMAKE_SOURCE_DIR}/tools/guest-probe/main.c"
        COMMENT "Cross-compiling the Windows guest probe"
        VERBATIM
    )
    add_custom_target(guest-probe ALL DEPENDS "${GUEST_PROBE_OUTPUT}")
else()
    message(STATUS "x86_64-w64-mingw32-g++ not found; the guest probe will not be built")
    add_custom_target(guest-probe
        COMMAND ${CMAKE_COMMAND} -E echo "guest-probe skipped: x86_64-w64-mingw32-g++ not found"
        VERBATIM
    )
endif()

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

# MemProcFS needs info.db and its shared libraries beside the executable, so the host runtime assets
# share bindir; the unprivileged client API lives under includedir/libdir.
file(GLOB MEMPROCFS_RUNTIME_LIBS "${CMAKE_SOURCE_DIR}/MemProcFS/*.so")

install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(TARGETS mpfshost_core ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(FILES ${MEMPROCFS_RUNTIME_LIBS} "${CMAKE_SOURCE_DIR}/MemProcFS/info.db"
        DESTINATION "${CMAKE_INSTALL_BINDIR}")
# The two directories ship empty but must exist beside the executable; see the post-build step above.
install(DIRECTORY DESTINATION "${CMAKE_INSTALL_BINDIR}/Symbols")
install(DIRECTORY DESTINATION "${CMAKE_INSTALL_BINDIR}/plugins")
install(FILES "${CMAKE_SOURCE_DIR}/README.md" DESTINATION "${CMAKE_INSTALL_DOCDIR}")
install(DIRECTORY "${CMAKE_SOURCE_DIR}/docs/" DESTINATION "${CMAKE_INSTALL_DOCDIR}"
        FILES_MATCHING PATTERN "*.md")
# The examples double as documentation: ship their README and sources next to the docs.
install(DIRECTORY "${CMAKE_SOURCE_DIR}/examples/" DESTINATION "${CMAKE_INSTALL_DOCDIR}/examples"
        FILES_MATCHING PATTERN "*.md" PATTERN "*.cpp" PATTERN "CMakeLists.txt")

install(FILES "${CMAKE_SOURCE_DIR}/src/core/client/client.hpp"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/mpfshost/core/client")
install(FILES "${CMAKE_SOURCE_DIR}/src/core/ipc/message.hpp" "${CMAKE_SOURCE_DIR}/src/core/ipc/frame.hpp"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/mpfshost/core/ipc")
install(FILES "${CMAKE_SOURCE_DIR}/src/core/version.hpp"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/mpfshost/core")

configure_file("${CMAKE_SOURCE_DIR}/packaging/mpfshost.pc.in"
               "${CMAKE_CURRENT_BINARY_DIR}/mpfshost.pc" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/mpfshost.pc"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")

configure_file("${CMAKE_SOURCE_DIR}/packaging/mpfshost.service"
               "${CMAKE_CURRENT_BINARY_DIR}/mpfshost.service" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/mpfshost.service"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/systemd/system")
