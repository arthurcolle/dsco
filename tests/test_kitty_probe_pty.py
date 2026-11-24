"""Exercise graphics selection against independent simulated terminals.

Usage: python3 tests/test_kitty_probe_pty.py ./build/test_kitty_graphics
"""
import os
import pty
import re
import select
import subprocess
import sys
import termios
import time
import unittest


class ProbePTYTest(unittest.TestCase):
    def test_exact_ack_and_tty_scope(self):
        pairs = [pty.openpty() for _ in range(3)]
        state = [termios.tcgetattr(s) for _, s in pairs]
        paths = [os.ttyname(s) for _, s in pairs]
        env = {k: v for k, v in os.environ.items()
               if k not in ("KITTY_WINDOW_ID", "DSCO_KITTY_GRAPHICS")}
        env.update(TERM="xterm-256color", TERM_PROGRAM="Apple_Terminal",
                   DSCO_KITTY_PROBE="force", DSCO_PIXEL_TUI="0")
        try:
            with subprocess.Popen([BINARY, paths[0], paths[0], *paths[1:]],
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                  stdin=subprocess.DEVNULL, env=env) as child:
                try:
                    for i, (master, _) in enumerate(pairs):
                        data = b""
                        until = time.monotonic() + 2
                        while b"\x1b[c" not in data:
                            wait = until - time.monotonic()
                            self.assertGreater(wait, 0, data)
                            self.assertTrue(select.select([master], [], [], wait)[0], data)
                            data += os.read(master, 4096)
                        match = re.search(rb"\x1b_Gi=([0-9]+),", data)
                        self.assertIsNotNone(match)
                        image = match[1]
                        if i == 0:
                            # Valid response with fragments crossing read boundaries.
                            os.write(master, b"\x1b_Gi=" + image)
                            time.sleep(0.025)
                            os.write(master, b";OK\x1b\\\x1b[?1;2c")
                        elif i == 1:
                            # A numeric prefix is not an exact image ID.
                            os.write(master, b"\x1b_Gi=" + image + b"9;OK\x1b\\\x1b[?1;2c")
                        else:
                            # Two separate responses cannot jointly constitute an ACK.
                            os.write(master, b"\x1b_Gi=" + image + b";EINVAL\x1b\\"
                                     b"\x1b_Gi=7;OK\x1b\\\x1b[?1;2c")
                    out, err = child.communicate(timeout=3)
                    self.assertEqual((child.returncode, err), (0, b""))
                    self.assertEqual(out.splitlines(), [b"1", b"1", b"0", b"0"])
                    self.assertEqual([termios.tcgetattr(s) for _, s in pairs], state)
                finally:
                    if child.poll() is None:
                        child.kill()
        finally:
            for pair in pairs:
                for fd in pair:
                    os.close(fd)


if __name__ == "__main__":
    BINARY = os.path.abspath(sys.argv.pop(1))
    unittest.main()
