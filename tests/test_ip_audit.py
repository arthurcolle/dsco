import hashlib
import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
import zipfile

SCRIPT = Path(__file__).resolve().parents[1] / "scripts/ip_audit.py"
# Synthetic restricted terms: the real list is never committed in clear text.
SALT = "0123456789abcdef"
HASHED_LINEAGE = "zorblax"
HASHED_SOURCE = "quux_frob notes"
PRIVATE_REGEX_TERM = "PRIVSYN-4711"


def synthetic_hash(term):
    normalized = " ".join(__import__("re").findall(r"[a-z0-9]+", term.lower()))
    return hashlib.sha256(f"{SALT}:{normalized}".encode()).hexdigest()


SPEC = importlib.util.spec_from_file_location("ip_audit", SCRIPT)
AUDIT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(AUDIT)


class IPDocumentationCheckTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="dsco-ip-check-")
        self.root = Path(self.temporary.name)
        subprocess.run(["git", "init", "-q", str(self.root)], check=True)
        config = Path(self.temporary.name + "-config")
        config.mkdir()
        self.addCleanup(lambda: __import__("shutil").rmtree(config, ignore_errors=True))
        self.hashes = config / "restricted.sha256"
        self.hashes.write_text(
            f"salt {SALT}\n"
            f"restricted_lineage {synthetic_hash(HASHED_LINEAGE)}\n"
            f"private_source {synthetic_hash(HASHED_SOURCE)}\n"
        )
        self.patterns = config / "patterns.txt"
        self.patterns.write_text("# synthetic\nrestricted_lineage: privsyn-\\d+\n")
        self.env = dict(os.environ, DSCO_IP_AUDIT_HASHES=str(self.hashes), DSCO_IP_AUDIT_PATTERNS=str(self.patterns))

    def tearDown(self):
        self.temporary.cleanup()

    def write(self, name, text, tracked=False):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
        if tracked:
            subprocess.run(["git", "-C", str(self.root), "add", "--", name], check=True)
        return path

    def run_check(self):
        return subprocess.run(["python3", str(SCRIPT), "--root", str(self.root), "--check"], capture_output=True, text=True, timeout=30, env=self.env)

    def test_local_implementation_and_external_library_notice_pass(self):
        self.write("docs/LANGUAGE.md", "Local Lua runtime; implementation in src/runtime.c.\n", tracked=True)
        self.write("src/runtime.c", "void runtime(void) {}\n", tracked=True)
        self.assertEqual(self.run_check().returncode, 0)

    def test_tracked_lineage_and_untracked_private_corpus_fail_without_quoting(self):
        self.write("docs/PRODUCT.md", "Zorblax inheritance is direct.\nPRIVATE_SENTINEL\n", tracked=True)
        self.write("reports/inventory.json", '{"path":"quux-frob/notes/IMG_XXXX.md"}')
        result = self.run_check()
        self.assertEqual(result.returncode, 1)
        self.assertIn("docs/PRODUCT.md:1", result.stderr)
        self.assertIn("reports/inventory.json:1", result.stderr)
        self.assertNotIn("PRIVATE_SENTINEL", result.stderr + result.stdout)

    def test_empty_reintroduced_source_map_fails(self):
        self.write("docs/lingo/ZORBLAX_SOURCE_MAP.md", "")
        self.assertEqual(self.run_check().returncode, 1)

    def test_private_pattern_file_detects_without_quoting(self):
        self.write("docs/NOTES.md", f"see {PRIVATE_REGEX_TERM} here\n", tracked=True)
        result = self.run_check()
        self.assertEqual(result.returncode, 1)
        self.assertIn("docs/NOTES.md:1: restricted_lineage", result.stderr)
        self.assertNotIn(PRIVATE_REGEX_TERM, result.stderr + result.stdout)

    def test_without_private_config_hashed_terms_still_detect(self):
        self.env["DSCO_IP_AUDIT_PATTERNS"] = str(self.root / "missing.txt")
        self.write("docs/PRODUCT.md", "built on ZORBLAX\n", tracked=True)
        self.assertEqual(self.run_check().returncode, 1)
        self.write("docs/PRODUCT.md", f"mentions {PRIVATE_REGEX_TERM} only\n", tracked=True)
        self.assertEqual(self.run_check().returncode, 0)

    def test_hash_terms_prints_hashes_not_terms(self):
        result = subprocess.run(["python3", str(SCRIPT), "--hash-terms", "private_source"], input="Quux-Frob  Notes\n",
                                capture_output=True, text=True, timeout=30, env=self.env)
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout.strip(), f"private_source {synthetic_hash(HASHED_SOURCE)}")

    def test_committed_hash_file_has_no_clear_text_terms(self):
        committed = SCRIPT.with_name("ip_audit_restricted.sha256").read_text().splitlines()
        body = [line for line in committed if line and not line.startswith("#")]
        self.assertTrue(body[0].startswith("salt "))
        for line in body[1:]:
            rule, digest = line.split()
            self.assertIn(rule, AUDIT.HASH_RULES)
            self.assertRegex(digest, r"^[0-9a-f]{64}$")

    def test_root_service_cannot_assert_unverified_independent_origin(self):
        self.write("service.py", 'note = "It is not a copy of any proprietary ontology."\n', tracked=True)
        result = self.run_check()
        self.assertEqual(result.returncode, 1)
        self.assertIn("unverified_origin_assurance", result.stderr)

    def test_archived_private_reference_fails_without_extracting_files(self):
        archive = self.root / "docs/package.zip"
        archive.parent.mkdir()
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as handle:
            handle.writestr("../../outside.md", "zorblax derived product")
        result = self.run_check()
        self.assertEqual(result.returncode, 1)
        self.assertIn("docs/package.zip:../../outside.md", result.stderr)
        self.assertFalse((self.root / "outside.md").exists())

    def test_audit_hashes_without_following_symlinks_or_reading_secret_names(self):
        target = self.write("docs/plain.md", "local implementation\n")
        (self.root / "docs/link.md").symlink_to(target)
        self.write("data/secrets.txt", "DO_NOT_QUOTE")
        destination = self.root / "output"
        subprocess.run(["git", "-C", str(self.root), "-c", "user.name=test", "-c", "user.email=test@example.invalid", "commit", "--allow-empty", "-qm", "fixture"], check=True)
        AUDIT.audit(self.root, destination)
        inventory = (destination / "inventory.jsonl").read_text()
        self.assertIn("symlink_not_followed", inventory)
        self.assertIn("sensitive_name_inventory_only", inventory)
        self.assertNotIn("DO_NOT_QUOTE", inventory)


if __name__ == "__main__":
    unittest.main()
