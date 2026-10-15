"""Pins for the WSL usbipd runbook.

These are static on purpose: they must pass on a machine with no Windows, no cable and
no board, because the thing they guard is that the doc and the script still agree.
The behavioural claims they describe were checked by hand on the bench; docs/wsl-usb-
reattach.md says which.

Run: python3 -m unittest discover -s tools/orb-manager/scripts
"""
import re
import subprocess
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
SCRIPT = HERE / "wsl-usb.sh"
DOC = HERE.parents[2] / "docs" / "wsl-usb-reattach.md"


class Script(unittest.TestCase):
    def setUp(self):
        self.src = SCRIPT.read_text()

    def test_syntax(self):
        subprocess.run(["bash", "-n", str(SCRIPT)], check=True, capture_output=True)

    @staticmethod
    def _verbs():
        block = SCRIPT.read_text().split("Verbs:\n",1)[1].split("\nEnv:",1)[0]
        return set(re.findall(r"^  ([a-z]+)\s", block, re.M))

    def test_every_verb_in_the_help_is_dispatched(self):
        dispatched = set(re.findall(r"^\s+([a-z]+)(?:\|[a-z0-9|\-]*)?\)\s", self.src, re.M))
        documented = self._verbs()
        self.assertTrue(documented, "help block lists no verbs")
        missing = documented - dispatched
        self.assertEqual(missing, set(), f"help advertises verbs with no dispatch: {missing}")

    def test_busid_is_never_hardcoded(self):
        # Windows moved a board from 1-10 to 1-6 inside one session. A literal busid in
        # this file is a bug, not a convenience.
        hits = re.findall(r"(?:busid[=\"' ]|remote=[^ ]+)\s*['\"]?(1(?:-[0-9]+))\b", self.src)
        self.assertEqual([h for h in hits if h != "1-6"] or hits, [], f"hardcoded busid: {hits}")

    def test_detach_is_client_side_first(self):
        body = self.src[self.src.index("cmd_detach()"):]
        body = body[: body.index("cmd_reset()")]
        self.assertLess(body.index("usbip detach"), body.index("detach --busid"),
                        "Windows-side detach goes first; a watcher answers it by re-taking the port")

    def test_a_missing_node_is_not_reported_present(self):
        # readlink -f prints the link path for a dangling link, so a released port would
        # otherwise look ready.
        body = self.src[self.src.index("node_of()"):]
        self.assertIn('[ -e "$t" ]', body[:400])

    def test_powershell_is_passed_base64(self):
        # bash -> powershell -> CIM quoting is where these helpers die.
        self.assertIn("-EncodedCommand", self.src)
        self.assertIn("utf-16-le", self.src)

    def test_the_service_is_never_killed_as_a_watcher(self):
        self.assertRegex(self.src, r"ProcessId -ne \$svc")


class Doc(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.doc = DOC.read_text()

    def test_doc_exists_and_names_the_script(self):
        self.assertIn("wsl-usb.sh", self.doc)

    def test_doc_covers_every_verb(self):
        verbs = sorted(Script._verbs())
        self.assertTrue(verbs)
        missing = [v for v in verbs if v not in self.doc]
        self.assertEqual(missing, [], f"verbs with no procedure in the doc: {missing}")

    def test_doc_uses_the_wsl_mount_of_windows(self):
        self.assertIn("/mnt/c/Windows/System32", self.doc)
        # The corrections section quotes the wrong path on purpose. Nothing else may.
        body = self.doc.split("## Three things")[0]
        self.assertNotIn("/c/Windows/System32", body.replace("/mnt/c/Windows/System32", ""),
                         "a /c path is not mountable from inside a distro")

    def test_doc_records_the_mirrored_requirement(self):
        self.assertIn("mirrored", self.doc)
        self.assertIn("USBIP_HOST", self.doc)

    def test_the_sudoers_rule_is_narrow(self):
        line = [l for l in self.doc.splitlines() if "NOPASSWD" in l]
        self.assertTrue(line)
        self.assertIn("/usr/bin/usbip", line[0])
        self.assertNotIn("ALL=(ALL)", line[0])
        for verb in re.findall(r"usbip (\w+)", line[0]):
            self.assertIn(verb, ("attach", "detach", "port", "devices"), verb)

    def test_the_unit_path_resolves(self):
        m = re.search(r"^ExecStart=(/.*)$", self.doc, re.M)
        self.assertTrue(m, "no absolute ExecStart in the unit")
        self.assertTrue(Path(m.group(1).split()[0]).exists(), m.group(1))

    def test_the_detach_line_is_the_script_not_bare_usbipd(self):
        # `usbipd detach` alone no longer releases a port once an auto-attach watcher is
        # registered, so the doc must not tell you to run it by itself.
        for para in re.split(r"\n\s*\n", self.doc):
            if "usbipd detach" in para:
                self.assertTrue("watch" in para or "replay" in para or "unwatch" in para,
                                para[:120])


if __name__ == "__main__":
    unittest.main()
