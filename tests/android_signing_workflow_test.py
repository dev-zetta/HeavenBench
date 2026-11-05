#!/usr/bin/env python3
"""Check the Android workflow's credential and artifact contracts without GitHub or keys."""

import ast
from itertools import product
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
FLAG = "HEAVEN_ANDROID_PERSISTENT_SIGNING"
TRUSTED = ("github.repository == 'dev-zetta/HeavenBench' && github.ref == 'refs/heads/main' && "
           "(github.event_name == 'push' || github.event_name == 'workflow_dispatch')")
SIGN = "Apply pinned persistent Android signature"
CANONICAL = "Upload persistently signed APK, matching source and public provenance"
DEVELOPMENT = "Upload development APK without distribution credentials"


def require(value, reason):
    if not value:
        raise ValueError(reason)


def expression(value):
    value = value.strip()
    if value.startswith("${{") and value.endswith("}}"):
        value = value[3:-2]
    return " ".join(value.split())


def field(block, name, indent):
    matches = re.findall(r"^" + " " * indent + re.escape(name) + r": (.+)$", block, re.MULTILINE)
    require(len(matches) == 1, "Missing or repeated workflow field: " + name)
    return matches[0]


def workflow_parts(text):
    # This test intentionally accepts only this small, reviewed workflow layout.
    # It is not a YAML parser or an independent check of GitHub Environment rules.
    sections = text.split("\njobs:\n")
    require(len(sections) == 2, "Expected one workflow jobs mapping")
    require(re.findall(r"^  ([\w-]+):", sections[1], re.MULTILINE) == ["apk"],
            "Expected exactly one Android job")
    sections = sections[1].split("\n    steps:\n")
    require(len(sections) == 2, "Expected one Android steps sequence")
    steps = re.split(r"(?m)^      - ", sections[1])[1:]
    require(steps, "Missing Android steps")
    return sections[0], steps


def named_step(steps, name):
    matches = [step for step in steps if step.startswith("name: " + name + "\n")]
    require(len(matches) == 1, "Missing or repeated workflow step: " + name)
    return matches[0]


def artifact_paths(step):
    match = re.search(r"^          path: \|\n((?:^            .*(?:\n|$))+)", step, re.MULTILINE)
    require(match, "Missing explicit artifact paths")
    return [line.strip() for line in match[1].splitlines()]


def check_contract(text):
    job, steps = workflow_parts(text)
    require("pull_request_target:" not in text and "workflow_run:" not in text,
            "Unreviewed privileged workflow trigger")
    require("contents: read" in text and not re.search(r"\bcontents:\s*write\b", text),
            "Android workflow must not publish or write repository contents")
    require(expression(field(job, "name", 6)) == TRUSTED + " && 'android-signing' || 'android-development'",
            "Environment selection differs from trusted-main policy")
    require(expression(field(job, FLAG, 6)) == TRUSTED,
            "Signing flag differs from trusted-main policy")
    signer = named_step(steps, SIGN)
    canonical = named_step(steps, CANONICAL)
    development = named_step(steps, DEVELOPMENT)
    positive = "env." + FLAG + " == 'true'"
    require(field(signer, "if", 8) == field(canonical, "if", 8) == positive,
            "Signing and canonical upload must share the fail-closed flag")
    require(field(development, "if", 8) == "env." + FLAG + " != 'true'",
            "Development upload must be mutually exclusive")
    outside_signer = text.replace("      - " + signer, "", 1)
    require(not re.search(r"\bsecrets\.[A-Za-z_]", outside_signer) and
            "HEAVEN_ANDROID_KEYSTORE_" not in outside_signer,
            "Signing credentials escaped the post-build signing step")
    require(field(signer, "HEAVEN_ANDROID_KEYSTORE_BASE64", 10) ==
            "${{ secrets.ANDROID_SIGNING_KEYSTORE_BASE64 }}" and
            field(signer, "HEAVEN_ANDROID_KEYSTORE_PASSWORD", 10) ==
            "${{ secrets.ANDROID_SIGNING_PASSWORD }}",
            "Missing explicit signing-step secret mapping")
    require(len(re.findall(r"\bsecrets\.[A-Za-z_]", signer)) == 2,
            "Unexpected signing-step secret reference")
    build = named_step(steps, "Build and validate ARM64 APK in Docker")
    require(steps.index(build) < steps.index(signer) < steps.index(canonical),
            "Signing and upload must follow the successful development build")
    require("./scripts/build_docker.sh android --abi arm64-v8a" in build,
            "Development build must retain the canonical Docker helper")
    versions = dict(line.split("=", 1) for line in (ROOT / "android/versions.properties").read_text().splitlines()
                    if line and not line.startswith("#"))
    for argument in ("python3 scripts/sign_android_apk.py", "--output-dir build/release/android-signed",
                     "--expected-certificate android/signing-certificate.sha256",
                     "--source-manifest build/release/android/source-manifest.json",
                     "--expected-version-code", "--expected-version-name", "--zipalign", "--aapt",
                     "sha256sum --check", "build-tools/" + versions["buildTools"]):
        require(argument in signer, "Missing signing input or verification: " + argument)
    for step, name, directory in ((canonical, "HeavenBench-android-arm64-v8a-debug", "android-signed"),
                                  (development, "HeavenBench-android-arm64-v8a-development", "android")):
        require(field(step, "name", 10) == name, "Unexpected Android artifact identity")
        paths = artifact_paths(step)
        require(all(path.startswith("build/release/" + directory + "/") for path in paths),
                "APK artifact crosses signed/development output boundary")
        require(any(path.endswith(".apk") for path in paths) and
                any(path.endswith(".apk.sha256") for path in paths) and
                any(path.endswith("source-manifest.json") for path in paths),
                "Missing APK, checksum or matching source manifest")
        require(field(step, "if-no-files-found", 10) == "error", "Missing APK must fail upload")
    require("build/release/android-signed/signing-certificate.json" in artifact_paths(canonical),
            "Canonical APK lacks public signing provenance")
    uploads = [step for step in steps if "uses: actions/upload-artifact@" in step]
    require(len(uploads) == 3, "Unexpected extra artifact upload")
    return job, signer, canonical, development


def evaluate(value, context):
    """Evaluate only the comparison/boolean subset used by the reviewed guards."""
    value = expression(value)
    for name in sorted(context, key=len, reverse=True):
        value = value.replace(name, repr(context[name]))
    tree = ast.parse(value.replace("&&", " and ").replace("||", " or "), mode="eval")

    def visit(node):
        if isinstance(node, ast.Constant) and isinstance(node.value, (str, bool)):
            return node.value
        if isinstance(node, ast.Compare) and len(node.ops) == 1 and len(node.comparators) == 1:
            left, right = visit(node.left), visit(node.comparators[0])
            if isinstance(node.ops[0], ast.Eq):
                return left == right
            if isinstance(node.ops[0], ast.NotEq):
                return left != right
        if isinstance(node, ast.BoolOp) and isinstance(node.op, (ast.And, ast.Or)):
            result = visit(node.values[0])
            for child in node.values[1:]:
                if (isinstance(node.op, ast.And) and result) or (isinstance(node.op, ast.Or) and not result):
                    result = visit(child)
            return result
        raise ValueError("Unreviewed workflow expression node")

    return visit(tree.body)


class AndroidSigningWorkflowTest(unittest.TestCase):
    def setUp(self):
        self.text = (ROOT / ".github/workflows/android.yml").read_text()

    def test_workflow_credential_and_artifact_contract(self):
        check_contract(self.text)

    @unittest.skipUnless(shutil.which("cmake"), "CMake is required to check host test registration")
    def test_cmake_signer_registration_is_linux_only(self):
        source = (ROOT / "CMakeLists.txt").read_text()
        registration = "if(Python3_Interpreter_FOUND)" + source.split("if(Python3_Interpreter_FOUND)", 1)[1]
        for system in ("Linux", "Darwin", "Windows"):
            with self.subTest(system=system), tempfile.TemporaryDirectory(prefix="heaven-signing-cmake-") as temporary:
                probe = Path(temporary) / "registration.cmake"
                # Execute the real conditional registration with add_test replaced
                # by a recorder; no project configure, compiler or tests run.
                probe.write_text('set(CMAKE_SYSTEM_NAME "' + system + '")\n'
                                 'set(Python3_Interpreter_FOUND TRUE)\n'
                                 'function(add_test)\n'
                                 '  set_property(GLOBAL APPEND PROPERTY REGISTERED_TESTS "${ARGV1}")\n'
                                 'endfunction()\n' + registration + '\n'
                                 'get_property(names GLOBAL PROPERTY REGISTERED_TESTS)\n'
                                 'message(STATUS "REGISTERED=${names}")\n')
                result = subprocess.run([shutil.which("cmake"), "-P", str(probe)],
                                        capture_output=True, text=True, timeout=15)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                names = re.search(r"REGISTERED=([^\r\n]+)", result.stdout)[1].split(";")
                self.assertIn("android_signing_workflow_regressions", names)
                self.assertEqual("android_signing_regressions" in names, system == "Linux")

    def test_trusted_main_event_matrix_selects_one_apk_artifact(self):
        job, signer, canonical, development = check_contract(self.text)
        for repository, ref, event in product(
                ("dev-zetta/HeavenBench", "fork/HeavenBench"),
                ("refs/heads/main", "refs/heads/topic", "refs/pull/1/merge", "refs/tags/main"),
                ("push", "workflow_dispatch", "pull_request", "pull_request_target")):
            with self.subTest(repository=repository, ref=ref, event=event):
                trusted = (repository == "dev-zetta/HeavenBench" and ref == "refs/heads/main" and
                           event in ("push", "workflow_dispatch"))
                context = {"github.repository": repository, "github.ref": ref, "github.event_name": event}
                flag = evaluate(field(job, FLAG, 6), context)
                self.assertIs(flag, trusted)
                self.assertEqual(evaluate(field(job, "name", 6), context),
                                 "android-signing" if trusted else "android-development")
                context["env." + FLAG] = str(flag).lower()
                self.assertIs(evaluate(field(signer, "if", 8), context), trusted)
                self.assertIs(evaluate(field(canonical, "if", 8), context), trusted)
                self.assertIs(evaluate(field(development, "if", 8), context), not trusted)

    def test_weakened_workflow_controls_are_rejected(self):
        controls = (
            ("github.ref == 'refs/heads/main' && ", ""),
            (FLAG + ": ${{ " + TRUSTED, FLAG + ": ${{ true || " + TRUSTED),
            ("if: env." + FLAG + " != 'true'", "if: env." + FLAG + " == 'true'"),
            ("if: env." + FLAG + " == 'true'", "if: always()"),
            ("build/release/android-signed/HeavenBench-", "build/release/android/HeavenBench-"),
            ("    env:\n", "    env:\n      LEAK: ${{ secrets.ANDROID_SIGNING_PASSWORD }}\n"),
            ("  pull_request:\n", "  pull_request_target:\n"),
            ("  apk:\n", "  extra:\n    runs-on: ubuntu-24.04\n  apk:\n"),
            ("sha256sum --check", "sha256sum"),
            ("--source-manifest build/release/android/source-manifest.json", ""),
        )
        for before, after in controls:
            with self.subTest(control=before):
                self.assertIn(before, self.text)
                with self.assertRaises(ValueError):
                    check_contract(self.text.replace(before, after, 1))


if __name__ == "__main__":
    unittest.main()
