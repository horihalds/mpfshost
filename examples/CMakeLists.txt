set(MPFSHOST_EXAMPLES
    list_targets
    list_processes
    read_process_memory
    scatter_read
    list_vfs
    guest_input
    special_keys
    keyboard_state
)

foreach(example IN LISTS MPFSHOST_EXAMPLES)
    add_executable(mpfshost_example_${example} ${example}.cpp)
    mpfshost_set_common_flags(mpfshost_example_${example})
    target_link_libraries(mpfshost_example_${example} PRIVATE mpfshost_client)
endforeach()
