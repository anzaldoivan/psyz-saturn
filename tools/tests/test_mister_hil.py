"""Contract tests for the opt-in MiSTer evidence runner. No hardware needed."""
import base64
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

from tools import mister_hil as hil


class CueTests(unittest.TestCase):
    def test_collects_cue_and_referenced_tracks(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / "test.cue").write_text('FILE "test.bin" BINARY\n  TRACK 01 MODE1/2352\n')
            (root / "test.bin").write_bytes(b"disc")
            self.assertEqual(
                [p.name for p in hil.cue_files(root / "test.cue")],
                ["test.cue", "test.bin"],
            )

    def test_rejects_traversal(self):
        with tempfile.TemporaryDirectory() as folder:
            cue = Path(folder) / "test.cue"
            cue.write_text('FILE "../secret.bin" BINARY\n')
            with self.assertRaises(hil.HILError):
                hil.cue_files(cue)

    def test_rejects_missing_track(self):
        with tempfile.TemporaryDirectory() as folder:
            cue = Path(folder) / "test.cue"
            cue.write_text('FILE "absent.bin" BINARY\n')
            with self.assertRaises(hil.HILError):
                hil.cue_files(cue)


class RpcTests(unittest.TestCase):
    @mock.patch.object(hil.request, "urlopen")
    def test_checks_response_and_propagates_method(self, urlopen):
        urlopen.return_value = io.BytesIO(
            json.dumps({"jsonrpc": "2.0", "id": "psyz-hil", "result": {"version": "test"}}).encode()
        )
        self.assertEqual(hil.rpc("http://mister.local:7497", "version"), {"version": "test"})
        sent = json.loads(urlopen.call_args.args[0].data)
        self.assertEqual(sent["method"], "version")
        self.assertEqual(sent["id"], "psyz-hil")

    @mock.patch.object(hil.request, "urlopen")
    def test_json_rpc_error_is_not_success(self, urlopen):
        urlopen.return_value = io.BytesIO(
            json.dumps({"jsonrpc": "2.0", "id": "psyz-hil", "error": {"message": "blocked"}}).encode()
        )
        with self.assertRaisesRegex(hil.HILError, "blocked"):
            hil.rpc("http://mister.local:7497", "run", {"text": "x"})

    @mock.patch.object(hil, "rpc")
    def test_screenshot_checks_png_signature(self, rpc):
        rpc.return_value = {"data": base64.b64encode(b"not png").decode()}
        with tempfile.TemporaryDirectory() as folder:
            with self.assertRaisesRegex(hil.HILError, "PNG"):
                hil.screenshot("http://mister.local:7497", Path(folder) / "shot.png")


class DeploymentTests(unittest.TestCase):
    @mock.patch.object(hil.subprocess, "run")
    def test_deploy_uses_ssh_scp_argv(self, subprocess_run):
        with tempfile.TemporaryDirectory() as folder:
            cue = Path(folder) / "test.cue"
            cue.write_text('FILE "test.bin" BINARY\n')
            track = Path(folder) / "test.bin"
            track.write_bytes(b"data")
            remote = hil.deploy("mister.local", "build_42", [cue, track])
            self.assertTrue(remote.endswith("/build_42/test.cue"))
            commands = [call.args[0][0] for call in subprocess_run.call_args_list]
            self.assertEqual(commands, ["ssh", "scp"])

    def test_rejects_unsafe_host(self):
        with self.assertRaises(hil.HILError):
            hil.deploy("mister.local;reboot", "smoke", [])


if __name__ == "__main__":
    unittest.main()
