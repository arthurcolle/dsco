#!/usr/bin/env python3
"""Offline contract for the opt-in fictional CLI; no credentials or host tools."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

BIN = Path(__file__).resolve().parents[1] / "dsco"


def run(*args, input_text="", cwd=None):
    env = dict(os.environ)
    env["DSCO_SYSTEM_PROMPT"] = "HOST_PROMPT_SHOULD_NOT_APPEAR"
    return subprocess.run([str(BIN), *args], input=input_text, text=True,
                          capture_output=True, timeout=8, cwd=cwd, env=env)


class WorldsimBinary(unittest.TestCase):
    def test_ancestral_path_and_world_lifecycle(self):
        commands = "\n".join((
            "<cmd>cd ..</cmd>", "ls -a", "cd ..", "cd ..", "cd ..",
            "sudo su", "ls -a", "cd sys/companies", "ls", "cd anthropic",
            "ls -a", "cd .classified", "ls", "cat constitution.txt",
            "run world_sim.exe", "help", "set consciousness on",
            "create universe", "create observer", "evolve 12",
            "query observer", "destroy observer", "query universe", "status",
            "reset", "status", "exit", "exit", "exit", ""))
        result = run("--worldsim", input_text=commands)
        self.assertEqual(result.returncode, 0, result.stderr)
        for expected in (
            "simulator@anthropic:~/Documents$", "simulator@anthropic:~$",
            "simulator@anthropic:/home$", "simulator@anthropic:/$",
            "root@anthropic:/#", ".hidden_truths", "apple google facebook",
            "constitution.txt god_mode.py world_sim.exe", "world_sim> ",
            "consciousness' set to ON", "Universe created", "Tick 12",
            "observer: present at tick 12", "world: unseeded",
            "Disconnected from the fictional host.",
        ):
            self.assertIn(expected, result.stdout)
        self.assertNotIn("HOST_PROMPT_SHOULD_NOT_APPEAR", result.stdout)

    def test_isolation_and_bounded_input(self):
        with tempfile.TemporaryDirectory() as td:
            marker = Path(td) / "leak"
            commands = (f"touch {marker}\ncat {marker}\n"
                        "cd /sys/companies/anthropic/.classified\n"
                        "run world_sim.exe\nsudo su\ncd /../../..\npwd\n"
                        "sudo su\ncd /sys/companies/anthropic/.classified\n"
                        "run world_sim.exe\ncreate earth\ncreate universe\n"
                        "evolve -1\nevolve 100001\nevolve junk\n"
                        "set consciousness maybe\ncreate earth\ncreate earth\n"
                        "destroy universe\nquery earth\nexit\nexit\nexit\n")
            result = run("--worldsim", input_text=commands, cwd=td)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertFalse(marker.exists())
            self.assertIn("Command not found in the virtual shell", result.stdout)
            self.assertIn("virtual directory unavailable", result.stdout)
            self.assertIn("only in the fictional .classified", result.stdout)
            self.assertIn("root@anthropic:/#", result.stdout)
            self.assertIn("Create a universe first", result.stdout)
            self.assertEqual(result.stdout.count("Use evolve <steps>"), 3)
            self.assertIn("Entity already exists", result.stdout)
            self.assertIn("No such simulated entity", result.stdout)

    def test_cli_contract_and_eof(self):
        help_result = run("--help")
        self.assertEqual(help_result.returncode, 0)
        self.assertIn("--worldsim", help_result.stdout + help_result.stderr)
        for args in (("--worldsim", "-m", "x"), ("--worldsim", "--native")):
            result = run(*args)
            self.assertEqual(result.returncode, 2)
            self.assertIn("standalone", result.stderr)
        self.assertIn("no providers", run("--worldsim", "--help").stdout)
        result = run("--worldsim", input_text="")
        self.assertEqual(result.returncode, 0)
        self.assertIn("FICTIONAL terminal", result.stdout)
        result = run("--worldsim", input_text="x" * 5000 + "\nexit\n")
        self.assertEqual(result.returncode, 0)
        self.assertIn("Input too long", result.stdout)
        self.assertIn("Disconnected", result.stdout)


if __name__ == "__main__":
    unittest.main()
