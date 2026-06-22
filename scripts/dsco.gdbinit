# dsco.gdbinit — live-debug / rescue hooks for dsco (Linux gdb).
#
# Usage:
#   gdb -x scripts/dsco.gdbinit --args ./dsco -i        # launch under gdb
#   gdb -x scripts/dsco.gdbinit -p $(pgrep -n dsco)      # attach to running
#
# dsco denies ptrace attach (prctl PR_SET_DUMPABLE 0) unless DSCO_DEBUG=1,
# DSCO_SUPERVISED, or DSCO_ALLOW_DEBUGGER is set. Export one first:
#   DSCO_DEBUG=1 ./dsco -i
#   ./dsco supervise -i     (supervised children already allow attach)

set pagination off
set print pretty on
set backtrace past-main on

# Stop on the fatal signals dsco's crash_handler traps so you land on the
# faulting frame. Let the control-flow signals pass through silently.
handle SIGSEGV stop print pass
handle SIGBUS  stop print pass
handle SIGABRT stop print pass
handle SIGILL  stop print pass
handle SIGFPE  stop print pass
handle SIGWINCH nostop noprint pass
handle SIGINT   nostop print pass
handle SIGTSTP  nostop noprint pass

break crash_handler

# Postmortem helper: full state in one command.
define dsco-bt
  thread apply all bt full
  info registers
  printf "g_main_interrupted = %d\n", g_main_interrupted
end
document dsco-bt
Dump every thread's backtrace, registers, and the agent interrupt flag.
end

# Live-rescue helper: clear the interrupt flag and let the loop continue.
define dsco-resume
  set variable g_main_interrupted = 0
  continue
end
document dsco-resume
Clear the interrupt flag and continue — recover a wedged agent loop.
end
