import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest
import zipfile

SCRIPT = Path(__file__).resolve().parents[1] / "scripts/ip_audit.py"
SPEC = importlib.util.spec_from_file_location("ip_audit", SCRIPT)
AUDIT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(AUDIT)


class IPDocumentationCheckTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="dsco-ip-check-")
        self.root = Path(self.temporary.name)
        subprocess.run(["git", "init", "-q", str(self.root)], check=True)

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
        return subprocess.run(["python3", str(SCRIPT), "--root", str(self.root), "--check"], capture_output=True, text=True, timeout=30)

    def test_local_implementation_and_external_library_notice_pass(self):
        self.write("docs/LANGUAGE.md", "Local Lua runtime; implementation in src/runtime.c.\n", tracked=True)
        self.write("src/runtime.c", "void runtime(void) {}\n", tracked=True)
        self.assertEqual(self.run_check().returncode, 0)

    def test_tracked_lineage_and_untracked_private_corpus_fail_without_quoting(self):
        self.write("docs/PRODUCT.md", "REDACTED_TERM inheritance is direct.\nPRIVATE_SENTINEL\n", tracked=True)
        self.write("reports/inventory.json", '{"path":"REDACTED_TERM/IMG_XXXX.md"}')
        result = self.run_check()
        self.assertEqual(result.returncode, 1)
        self.assertIn("docs/PRODUCT.md:1", result.stderr)
        self.assertIn("reports/inventory.json:1", result.stderr)
        self.assertNotIn("PRIVATE_SENTINEL", result.stderr + result.stdout)

    def test_empty_reintroduced_source_map_fails(self):
        self.write("docs/lingo/REDACTED_TERM_SOURCE_MAP.md", "")
        self.assertEqual(self.run_check().returncode, 1)

    def test_archived_private_reference_fails_without_extracting_files(self):
        archive = self.root / "docs/package.zip"
        archive.parent.mkdir()
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as handle:
            handle.writestr("../../outside.md", "REDACTED_TERM derived product")
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
