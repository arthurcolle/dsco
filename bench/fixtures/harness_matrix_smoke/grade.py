from pathlib import Path
import subprocess
import sys

target = Path("fizzbuzz.py")
if not target.is_file():
    raise SystemExit("fizzbuzz.py is missing")
completed = subprocess.run([sys.executable, str(target)], capture_output=True, text=True, timeout=5)
expected = ["FizzBuzz" if number % 15 == 0 else "Fizz" if number % 3 == 0 else
            "Buzz" if number % 5 == 0 else str(number) for number in range(1, 31)]
observed = completed.stdout.splitlines()
if completed.returncode != 0 or observed != expected:
    raise SystemExit(f"unexpected output: exit={completed.returncode} lines={observed!r}")
