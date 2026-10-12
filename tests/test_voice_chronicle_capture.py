#!/usr/bin/env python3
"""One bounded embedded-voice overflow case through the owned terminal fixture."""
import os
from pathlib import Path
import sys
import tempfile
from test_voice_session import run

fixture = Path(sys.argv[1]).resolve(strict=True)
with tempfile.TemporaryDirectory(prefix="dsco-voice-capture-") as home:
    os.environ["DSCO_VOICE_FIXTURE_TRUNCATE"] = "1"
    outcome = run(fixture, "tui", "spoken", home)
    assert outcome["ok"]
print("PASS: embedded voice persisted failed status and explicit bounded text truncation")
