# Sourced by the stack samplers and profile_ops.sh.
#   ptrace_remedy   prints why this user may be unable to attach to a process that is not its
#                   child, and the fix; prints nothing when attaching should work.
# A process that calls prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY) can be attached under
# ptrace_scope 1; gui_bench.py does that when GUI_BENCH_PTRACER_ANY=1.

ptrace_scope() {
    cat /proc/sys/kernel/yama/ptrace_scope 2> /dev/null || echo none
}

ptrace_remedy() {
    local scope
    scope=$(ptrace_scope)
    case $scope in
        none|0) ;;
        1)
            [ "$(id -u)" = 0 ] && return 0
            echo "kernel.yama.ptrace_scope is 1: only an ancestor, or a tracer the target names with prctl(PR_SET_PTRACER), may attach." \
                "The bench names any tracer when GUI_BENCH_PTRACER_ANY=1; for other targets run: sudo sysctl kernel.yama.ptrace_scope=0"
            ;;
        2)
            [ "$(id -u)" = 0 ] && return 0
            echo "kernel.yama.ptrace_scope is 2: only root may attach. Run: sudo sysctl kernel.yama.ptrace_scope=1" \
                "(the bench then lets the sampler attach) and set it back to 2 afterwards, or run the profile as root"
            ;;
        *)
            echo "kernel.yama.ptrace_scope is $scope: attaching is disabled until reboot"
            ;;
    esac
}
