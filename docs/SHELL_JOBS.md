# Shell execution and durable jobs

Foreground `bash` now defaults to 600 seconds. Explicit shorter timeouts remain shorter. Quiet foreground commands receive their full wall budget; no separate 60-second idle kill applies. Foreground maximum remains 600 seconds.

Use the existing governed `bash` tool:

```json
{"command":"make dsco -j8", "run_in_background":true, "timeout":600}
{"action":"list"}
{"action":"status", "job_id":"job-ABC123"}
{"action":"output", "job_id":"job-ABC123", "offset":0, "max_bytes":8192}
{"action":"wait", "job_id":"job-ABC123", "wait_seconds":2}
{"action":"cancel", "job_id":"job-ABC123"}
```

Jobs persist in `$HOME/.dsco/shell-jobs/job-*`: request.json, atomic status.json, output.log, supervisor.lock and optional cancel marker. A detached, re-executed supervisor owns/reaps the shell process group. New clients can poll without the original session. Cancellation uses a marker, not an arbitrary PID. Background wall timeout defaults to 600 seconds and accepts 1..86400 seconds. Wait calls cap at 30 seconds. Logs cap at 64 MiB, reads at 64 KiB (or available response budget); continue at next_offset. List caps at 64 jobs and reports truncation. Finished receipts/logs remain until explicitly cleaned up by the owner.

States: starting, running, completed, failed, cancelled, timed_out, interrupted. Successful launch is NOT successful command completion. A completed shell is NOT artifact verification: verify outputs separately before reporting task completion. Background requests do not perform the foreground artifact contract. Request files contain shell commands; do not embed secrets in commands. All dispatch still uses the existing execution capability gate. Bash/BashOutput/KillShell compatibility calls support durable job IDs; old in-memory IDs retain their existing behavior.

The supervisor is not a system launch daemon: reboot, SIGKILL, or deliberately escaped process groups are not recovery guarantees. Dead supervisors are reported interrupted when detected, not successful. Existing agent processes keep their old executable until restarted. Changes to a hosted/provider tool-call deadline are outside this local implementation.

Regression: `python3 tests/test_shell_jobs.py ./dsco` (about 70 seconds), plus `bash tests/verify_gate_claims.sh ./dsco`.
