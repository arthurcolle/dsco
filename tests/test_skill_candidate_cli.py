#!/usr/bin/env python3
"""Black-box tests for the explicit, unverified skill-candidate packager.

Run after building the CLI:
    python3 tests/test_skill_candidate_cli.py [./dsco] [-v]
    DSCO_TEST_BINARY=/path/to/dsco python3 tests/test_skill_candidate_cli.py -v

A positional binary path takes precedence over DSCO_TEST_BINARY. No model calls,
installation, promotion, or evaluator-policy changes are part of this suite.
"""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


BINARY = Path(os.environ.get("DSCO_TEST_BINARY", "./dsco")).expanduser().resolve()


def valid_episode():
    return {
        "name": "check-local-artifact",
        "goal": "Verify a local artifact without granting new authority.",
        "procedure": ["Read the selected artifact.", "Compare its bytes with the fixture."],
        "acceptance": ["The bytes match the expected fixture."],
        "evidence": ["fixture://local-artifact/run-1"],
    }


class SkillCandidateCLITests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not BINARY.is_file() or not os.access(BINARY, os.X_OK):
            raise RuntimeError("Build dsco or set DSCO_TEST_BINARY to an executable: %s" % BINARY)

    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix="dsco-skill-candidate-")
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.source = self.root / "selected episode.json"
        self.output = self.root / "new candidate"
        self.home = self.root / "home"
        self.home.mkdir()
        self.workspace = self.home / ".dsco" / "workspace"
        self.env = os.environ.copy()
        self.env.update({"HOME": str(self.home), "DSCO_WORKSPACE": str(self.workspace),
                         "DSCO_PRICING_OFFLINE": "1"})

    def write_episode(self, episode):
        # Deliberately noncanonical bytes: the source must be copied, not reserialized.
        raw = ("\n" + json.dumps(episode, ensure_ascii=False, indent=3) + "\n\n").encode("utf-8")
        self.source.write_bytes(raw)
        return raw

    def invoke(self, source=None, output=None):
        result = subprocess.run(
            [str(BINARY), "learn", "from", str(source or self.source), str(output or self.output)],
            cwd=self.root,
            env=self.env,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            encoding="utf-8",
            errors="replace",
            timeout=20,
            check=False,
        )
        # A signal/crash must not count as successful validation of a bad input.
        self.assertGreaterEqual(result.returncode, 0, self.diagnostic(result))
        return result

    @staticmethod
    def diagnostic(result):
        return "exit=%s\nstdout:\n%s\nstderr:\n%s" % (
            result.returncode, result.stdout, result.stderr
        )

    def assert_no_install(self):
        skills = self.workspace / "skills"
        self.assertFalse(skills.exists() and any(skills.iterdir()), "candidate was installed")

    def assert_rejected(self):
        original = self.source.read_bytes() if self.source.exists() else None
        result = self.invoke()
        self.assertNotEqual(result.returncode, 0, self.diagnostic(result))
        self.assertFalse(self.output.exists(), "rejected input left a candidate directory")
        if original is not None:
            self.assertEqual(self.source.read_bytes(), original, "source was modified")
        self.assert_no_install()

    def assert_candidate(self, episode):
        original = self.write_episode(episode)
        result = self.invoke()
        self.assertEqual(result.returncode, 0, self.diagnostic(result))
        self.assertTrue(self.output.is_dir())
        for name in ("SKILL.md", "episode.json", "manifest.json"):
            with self.subTest(artifact=name):
                artifact = self.output / name
                self.assertTrue(artifact.is_file(), "missing artifact: %s" % artifact)
                self.assertGreater(artifact.stat().st_size, 0)
        self.assertEqual((self.output / "episode.json").read_bytes(), original)
        self.assertEqual(self.source.read_bytes(), original)
        self.assertEqual(json.loads((self.output / "episode.json").read_text()), episode)
        markdown = (self.output / "SKILL.md").read_text(encoding="utf-8")
        for text in [episode["name"], episode["goal"]] + episode["procedure"] + episode["acceptance"]:
            with self.subTest(skill_text=text):
                self.assertIn(text, markdown)
        manifest = json.loads((self.output / "manifest.json").read_text(encoding="utf-8"))
        self.assertIsInstance(manifest, dict)
        self.assertEqual(manifest.get("status"), "candidate")
        self.assertIs(manifest.get("promotion_eligible"), False)
        self.assertIn("schema_version", manifest)
        self.assertTrue(manifest["schema_version"])
        # The slice contract specifies the meaning, not the spelling of this field.
        self.assertTrue(
            manifest.get("acceptance_status") == "unverified"
            or manifest.get("acceptance_verified") is False,
            "manifest must explicitly identify acceptance as unverified",
        )
        self.assert_no_install()
        return markdown, manifest

    def test_valid_required_fields_create_candidate_artifacts(self):
        self.assert_candidate(valid_episode())

    def test_valid_optional_fields_and_unicode_are_preserved(self):
        episode = valid_episode()
        episode.update({
            "when_to_use": "When inspecting an operator-selected café fixture.",
            "limitations": "Local evidence only; no verified transfer or authority.",
        })
        markdown, _ = self.assert_candidate(episode)
        self.assertIn(episode["when_to_use"], markdown)
        self.assertIn(episode["limitations"], markdown)

    def test_episode_procedure_is_data_not_executed(self):
        marker = self.root / "must-not-be-created"
        episode = valid_episode()
        episode["procedure"] = ["touch '%s'" % marker]
        self.assert_candidate(episode)
        self.assertFalse(marker.exists(), "episode procedure was executed")

    def test_missing_required_fields(self):
        for field in ("name", "goal", "procedure", "acceptance", "evidence"):
            with self.subTest(missing=field):
                episode = valid_episode()
                del episode[field]
                self.write_episode(episode)
                self.assert_rejected()

    def test_invalid_name_and_goal_types(self):
        for field in ("name", "goal"):
            for value in (None, False, 17, [], {}):
                with self.subTest(field=field, value=value):
                    episode = valid_episode()
                    episode[field] = value
                    self.write_episode(episode)
                    self.assert_rejected()

    def test_empty_goal(self):
        episode = valid_episode()
        episode["goal"] = ""
        self.write_episode(episode)
        self.assert_rejected()

    def test_unsafe_names(self):
        for name in ("", "../escape", "a/b", "/absolute", ".", "..", "Uppercase",
                     "has space", "has_underscore", "bad\nname", "bad\\name", "café"):
            with self.subTest(name=name):
                episode = valid_episode()
                episode["name"] = name
                self.write_episode(episode)
                self.assert_rejected()

    def test_invalid_procedure_acceptance_and_evidence_arrays(self):
        invalid = (None, False, 7, "not an array", {}, [], [None], [False], [7],
                   [{}], [[]], [""], ["valid first item", ""], ["valid first item", 7])
        for field in ("procedure", "acceptance", "evidence"):
            for value in invalid:
                with self.subTest(field=field, value=value):
                    episode = valid_episode()
                    episode[field] = value
                    self.write_episode(episode)
                    self.assert_rejected()

    def test_invalid_optional_field_types(self):
        for field in ("when_to_use", "limitations"):
            for value in (None, False, 7, [], {}):
                with self.subTest(field=field, value=value):
                    episode = valid_episode()
                    episode[field] = value
                    self.write_episode(episode)
                    self.assert_rejected()

    def test_non_object_json(self):
        for value in (None, False, 7, "episode", [], [valid_episode()]):
            with self.subTest(value=value):
                self.write_episode(value)
                self.assert_rejected()

    def test_malformed_json(self):
        valid = json.dumps(valid_episode()).encode("utf-8")
        for raw in (b"", b"not json", b'{"name":', b"{'name':'bad'}",
                    valid[:-1] + b",}", valid + b" trailing-garbage"):
            with self.subTest(raw=raw):
                self.source.write_bytes(raw)
                self.assert_rejected()

    def test_non_existing_input(self):
        self.assertFalse(self.source.exists())
        self.assert_rejected()
        self.assertFalse(self.source.exists())

    @staticmethod
    def tree_snapshot(directory):
        snapshot = {}
        for path in [directory] + sorted(directory.rglob("*")):
            info = path.stat()
            snapshot[str(path.relative_to(directory))] = (
                info.st_mode, info.st_ino, info.st_mtime_ns,
                path.read_bytes() if path.is_file() else None,
            )
        return snapshot

    def test_existing_output_directory_is_untouched(self):
        original = self.write_episode(valid_episode())
        self.output.mkdir()
        for name in ("SKILL.md", "episode.json", "manifest.json"):
            (self.output / name).write_bytes(b"existing artifact: " + name.encode("ascii"))
        (self.output / "nested").mkdir()
        (self.output / "nested" / "sentinel").write_bytes(b"keep me\x00\xff")
        before = self.tree_snapshot(self.output)
        result = self.invoke()
        self.assertNotEqual(result.returncode, 0, self.diagnostic(result))
        self.assertEqual(self.tree_snapshot(self.output), before)
        self.assertEqual(self.source.read_bytes(), original)
        self.assert_no_install()

    def test_existing_empty_output_directory_is_untouched(self):
        self.write_episode(valid_episode())
        self.output.mkdir()
        before = self.tree_snapshot(self.output)
        result = self.invoke()
        self.assertNotEqual(result.returncode, 0, self.diagnostic(result))
        self.assertEqual(self.tree_snapshot(self.output), before)
        self.assert_no_install()

    def test_existing_output_file_is_untouched(self):
        self.write_episode(valid_episode())
        self.output.write_bytes(b"not a directory\x00\xff")
        before = self.output.stat()
        result = self.invoke()
        self.assertNotEqual(result.returncode, 0, self.diagnostic(result))
        self.assertEqual(self.output.read_bytes(), b"not a directory\x00\xff")
        after = self.output.stat()
        self.assertEqual((after.st_ino, after.st_mtime_ns), (before.st_ino, before.st_mtime_ns))
        self.assert_no_install()

    def verify(self):
        return subprocess.run([str(BINARY), "learn", "verify", str(self.output)],
                              cwd=self.root, env=self.env, stdin=subprocess.DEVNULL,
                              capture_output=True, text=True, timeout=20)

    def test_verify_valid_is_read_only_and_not_acceptance(self):
        self.write_episode(valid_episode())
        self.assertEqual(self.invoke().returncode, 0)
        before = self.tree_snapshot(self.output)
        result = self.verify()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout), {
            "integrity": "verified", "acceptance_status": "unverified",
            "promotion_eligible": False})
        self.assertEqual(self.tree_snapshot(self.output), before)
        self.assert_no_install()

    def test_verify_rejects_tampering_each_artifact(self):
        self.write_episode(valid_episode())
        self.assertEqual(self.invoke().returncode, 0)
        for name in ("episode.json", "SKILL.md", "manifest.json"):
            with self.subTest(name=name):
                path = self.output / name
                original = path.read_bytes()
                path.write_bytes(original + b" ")
                self.assertNotEqual(self.verify().returncode, 0)
                path.write_bytes(original)

    def test_verify_rejects_missing_and_symlink_artifacts(self):
        self.write_episode(valid_episode())
        self.assertEqual(self.invoke().returncode, 0)
        for name in ("episode.json", "SKILL.md", "manifest.json"):
            with self.subTest(name=name):
                path = self.output / name
                original = path.read_bytes()
                path.unlink()
                self.assertNotEqual(self.verify().returncode, 0)
                target = self.root / (name + ".outside")
                target.write_bytes(original)
                path.symlink_to(target)
                self.assertNotEqual(self.verify().returncode, 0)
                path.unlink()
                path.write_bytes(original)

    def test_verify_rejects_nonexistent_directory(self):
        self.assertNotEqual(self.verify().returncode, 0)
        self.assertFalse(self.output.exists())


if __name__ == "__main__":
    if len(sys.argv) > 1 and not sys.argv[1].startswith("-"):
        BINARY = Path(sys.argv.pop(1)).expanduser().resolve()
    unittest.main()
