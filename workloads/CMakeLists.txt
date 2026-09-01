# Synthetic benchmark workloads (docs/design-draft-v2.md §21).
# Each subdirectory gets a `main.c` in Phase 4/5; until then this is a no-op.
#
#   sparse_random/       touches a small random subset      -> tests lazy
#   dense_scan/          touches nearly every page          -> tests eager
#   stable_hot_set/      reuses one subset across restores  -> tests prefetch
#   sudden_phase_change/ shifts working set mid-run         -> tests selection

foreach(wl sparse_random dense_scan stable_hot_set sudden_phase_change)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${wl}/main.c)
        add_executable(workload_${wl} ${wl}/main.c)
        set_target_properties(workload_${wl} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/workloads)
    endif()
endforeach()
