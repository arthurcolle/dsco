#!/usr/bin/env python3
"""Real governed CLI, separate clients, retained jobs; local fixtures only."""
import argparse
import json
import os
import signal
from pathlib import Path
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", default="./dsco")
    parser.add_argument("--artifact-dir", default="build/job-events")
    options = parser.parse_args()
    binary = str(Path(options.binary).resolve())
    artifacts = Path(options.artifact_dir).resolve()
    artifacts.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="dsco-job-lifecycle-") as directory:
        home = Path(directory)
        env = {key: value for key, value in os.environ.items() if not key.startswith(("DSCO_", "TOOLS_API_"))}
        env.update(HOME=str(home), DSCO_NO_AUTO_SUPERVISE="1", DSCO_PRICING_OFFLINE="1",
                   DSCO_SECURE_STORE_NO_PROMPT="1", DSCO_GOV_MODEL="standard")
        checks = []
        jobs = []

        def call(arguments, tool="bash", overrides=None):
            process = subprocess.run([binary, "--tool-exec-raw", tool, json.dumps(arguments)],
                                     cwd=home, env=dict(env, **(overrides or {})),
                                     capture_output=True, text=True, timeout=45)
            assert process.returncode == 0, (process.returncode, process.stdout, process.stderr)
            return json.loads(process.stdout)

        def start(command, **arguments):
            result = call(dict(command=command, run_in_background=True, **arguments))
            jobs.append(result["job_id"])
            return result

        def wait(job, seconds=10):
            return call(dict(action="wait", job_id=job, wait_seconds=seconds))

        try:
            job = start("sleep 3; printf owned-completion")
            assert job["execution_timeout_seconds"] == 0 and job["deadline_policy"] == "completion", job
            pending = wait(job["job_id"], 1)
            assert pending["status"] == "running", pending
            # Terminate a separately owned waiting client. The supervisor remains.
            client = subprocess.Popen([binary, "--tool-exec-raw", "bash",
                                       json.dumps(dict(action="wait", job_id=job["job_id"], wait_seconds=30))],
                                      cwd=home, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            time.sleep(.15)
            client.terminate()
            client.communicate(timeout=5)
            completed = wait(job["job_id"])
            assert completed["status"] == "completed" and completed["output"] == "owned-completion", completed
            assert completed["execution_timeout_seconds"] == 0 and completed["event_waits"] <= 6, completed
            assert completed["wait_backend"] == "kqueue", completed
            checks.append(dict(name="completion-owned lifetime and caller disconnect", result=completed))
            assert call(dict(action="output", job_id=job["job_id"], offset=6, max_bytes=3))["output"] == "com"
            checks.append(dict(name="retained output offsets across separate clients"))

            job = start("sleep 30")
            cancelled_at = time.monotonic()
            cancelled = call(dict(action="cancel", job_id=job["job_id"]))
            assert cancelled["status"] == "cancelled", cancelled
            checks.append(dict(name="explicit cancel wakes idle supervisor", elapsed_s=round(time.monotonic()-cancelled_at, 3), result=cancelled))

            job = start("trap '' TERM; printf ready; sleep 30")
            # The running receipt precedes shell execution. Await actual ready
            # output using a bounded observation, so the TERM-ignore fixture
            # is active before the cancellation request.
            ready = wait(job["job_id"], 1)
            assert ready["output"] == "ready", ready
            cancelled_at = time.monotonic()
            escalated = call(dict(action="cancel", job_id=job["job_id"]))
            assert escalated["status"] == "cancelled" and escalated["signal"] == 9, escalated
            checks.append(dict(name="one scheduled TERM-to-KILL escalation", elapsed_s=round(time.monotonic()-cancelled_at, 3), result=escalated))

            job = start("sleep 30")
            os.kill(job["supervisor_pid"], signal.SIGTERM)
            assert wait(job["job_id"])["status"] == "cancelled"
            checks.append(dict(name="control signal wakes supervisor"))

            job = start("sleep 30", timeout_seconds=1)
            expired = wait(job["job_id"])
            assert expired["status"] == "timed_out" and expired["execution_timeout_seconds"] == 1, expired
            checks.append(dict(name="explicit deadline retained", result=expired))

            configured = call(dict(command="sleep 30", run_in_background=True), overrides={"DSCO_COMMAND_TIMEOUT_S": "1"})
            jobs.append(configured["job_id"])
            assert wait(configured["job_id"])["status"] == "timed_out"
            checks.append(dict(name="explicit operator budget retained"))

            completed = wait(start("printf zero-deadline", timeout_seconds=0)["job_id"])
            assert completed["status"] == "completed" and completed["output"] == "zero-deadline"
            checks.append(dict(name="zero deadline is completion-driven"))

            aliased = call(dict(command="printf compatible", run_in_background=True, timeout_seconds=0), tool="Bash")
            jobs.append(aliased["job_id"])
            assert wait(aliased["job_id"])["status"] == "completed"
            assert call(dict(bash_id=aliased["job_id"]), tool="BashOutput")["output"] == "compatible"
            checks.append(dict(name="Bash and BashOutput aliases retain durable identity"))

            for index in range(12):
                completed = wait(start("printf quick-exit")["job_id"])
                assert completed["status"] == "completed" and completed["output"] == "quick-exit", (index, completed)
            checks.append(dict(name="fast exit and publication races", iterations=12))

            denied = subprocess.run([binary, "--tool-exec-raw", "bash", json.dumps(dict(command="printf denied", run_in_background=True))],
                                    cwd=home, env=dict(env, DSCO_ALLOW_RUN="0"), capture_output=True, text=True, timeout=10)
            assert denied.returncode != 0 and "disabled" in denied.stdout.lower(), (denied.stdout, denied.stderr)
            checks.append(dict(name="capability gate denies before spawn"))
            result = dict(schema="dsco.shell_job_events.proof.v1", binary=binary, checks=checks, paid_requests=0, provider_requests=0)
            (artifacts/"results.json").write_text(json.dumps(result, indent=2)+"\n")
            print(json.dumps(result))
        finally:
            for job_id in jobs:
                try:
                    status = call(dict(action="status", job_id=job_id))
                    if status["status"] in ("starting", "running"):
                        call(dict(action="cancel", job_id=job_id))
                except (AssertionError, subprocess.TimeoutExpired):
                    pass


if __name__ == "__main__":
    main()
