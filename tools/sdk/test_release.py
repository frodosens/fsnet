"""Offline regression checks for the SDK publisher's draft lifecycle."""
import contextlib
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import release


class PublisherTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.source = "a" * 40
        self.tag = "sdk-v0.3.1"
        self.asset = self.root / "SDK.zip"
        self.asset.write_bytes(b"verified producer bytes")

    def uploaded(self, digest=None):
        return {"id": 42, "tag_name": self.tag, "draft": True, "assets": [{
            "name": self.asset.name, "state": "uploaded", "size": self.asset.stat().st_size,
            "digest": digest,
        }]}

    def test_draft_id_and_delayed_digest(self):
        ready = self.uploaded("sha256:" + release.digest(self.asset))
        with patch.object(release, "gh_json", side_effect=[self.uploaded(), ready]) as api, \
                patch.object(release.time, "sleep") as wait:
            release.verify_uploaded("owner/repo", 42, self.tag, [self.asset])
        self.assertEqual(api.call_count, 2)
        self.assertEqual(api.call_args.args, ("repos/owner/repo/releases/42",))
        wait.assert_called_once_with(2)

    def test_wrong_digest_fails_without_retry(self):
        with patch.object(release, "gh_json", return_value=self.uploaded("sha256:" + "0" * 64)), \
                patch.object(release.time, "sleep") as wait:
            with self.assertRaisesRegex(RuntimeError, "server-side digest verification"):
                release.verify_uploaded("owner/repo", 42, self.tag, [self.asset])
        wait.assert_not_called()

    def test_pending_digest_has_bounded_retries(self):
        with patch.object(release, "gh_json", return_value=self.uploaded()) as api, \
                patch.object(release.time, "sleep") as wait:
            with self.assertRaisesRegex(RuntimeError, "did not finish"):
                release.verify_uploaded("owner/repo", 42, self.tag, [self.asset], attempts=3)
        self.assertEqual(api.call_count, 3)
        self.assertEqual(wait.call_count, 2)

    def test_publish_retains_created_draft_id(self):
        output = self.root / "output"
        output.mkdir()
        files = [output / name for name in ("SDK.zip", "SHA256SUMS", "sdk-release-manifest.json")]
        for path in files:
            path.write_bytes(b"verified " + path.name.encode())
        calls = []

        def api(endpoint, method="GET", **kwargs):
            calls.append((endpoint, method, kwargs))
            if endpoint.endswith("/git/ref/tags/" + self.tag):
                return None
            if endpoint.endswith("/git/refs"):
                return {"object": {"sha": self.source}}
            if endpoint == "repos/owner/repo/releases":
                self.assertIs(kwargs["payload"]["draft"], True)
                self.assertEqual(kwargs["payload"]["target_commitish"], self.source)
                return {"id": 42, "tag_name": self.tag, "draft": True}
            if endpoint.startswith("https://uploads.github.com/"):
                self.assertIn("/releases/42/assets?name=", endpoint)
                self.assertIn(kwargs["input_file"], files)
                return {}
            self.assertEqual(endpoint, "repos/owner/repo/releases/42")
            if method == "PATCH":
                self.assertIs(kwargs["payload"]["draft"], False)
                return {"id": 42, "tag_name": self.tag, "draft": False}
            return {"id": 42, "tag_name": self.tag, "draft": True, "assets": [{
                "name": path.name, "state": "uploaded", "size": path.stat().st_size,
                "digest": "sha256:" + release.digest(path),
            } for path in files]}

        with patch.object(release, "verify_run"), \
                patch.object(release, "verify_assets", return_value=[{"name": "SDK.zip"}]), \
                patch.object(release, "gh_json", side_effect=api), contextlib.redirect_stdout(io.StringIO()):
            release.publish("owner/repo", 1, self.source, "0.3.1", self.root, output)
        self.assertEqual(sum("uploads.github.com" in call[0] for call in calls), 3)
        self.assertEqual(calls[-1][1], "PATCH")
        self.assertFalse(any("releases/tags/" in call[0] for call in calls))

    def test_gh_json_preserves_boolean_payload(self):
        with patch.object(release.subprocess, "run") as run:
            run.return_value.returncode = 0
            run.return_value.stdout = "{}"
            release.gh_json("repos/owner/repo/releases", method="POST", payload={"draft": True})
        self.assertEqual(json.loads(run.call_args.kwargs["input"]), {"draft": True})
        self.assertIn("--input", run.call_args.args[0])


if __name__ == "__main__":
    unittest.main()
