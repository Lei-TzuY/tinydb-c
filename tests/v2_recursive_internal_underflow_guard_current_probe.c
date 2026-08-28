/*
 * Reuse the established height-4 fixture, but keep the fail-closed regression
 * on the final orientation that remains unsupported. Deletes 10, 20, 30, 40,
 * 50, 60, and 80 now have explicit height-contraction routes; deleting key 70
 * (the left leaf of the right bottom parent under the right grandparent) needs
 * the last mirrored lower-merge orientation and must still fail without
 * mutating topology or allocator state.
 */
#define main tinydb_legacy_recursive_underflow_probe_main
#include "v2_recursive_internal_underflow_guard_probe.c"
#undef main

int main(int argc, char** argv) {
    if (argc != 2) return EXIT_FAILURE;
    char path[1024];
    if (snprintf(path, sizeof(path), "%s.unsupported", argv[1]) < 0) {
        return EXIT_FAILURE;
    }
    if (!run_case(path, 70u)) return EXIT_FAILURE;
    printf("V2_RECURSIVE_INTERNAL_UNDERFLOW_GUARD_OK "
           "unsupported_right_outer_left=yes height4=yes fail_closed=yes "
           "root_stable=yes ancestor_stable=yes control_subtree=yes "
           "leaf_chain=yes allocator=yes reopen=yes integrity=yes\n");
    return EXIT_SUCCESS;
}
