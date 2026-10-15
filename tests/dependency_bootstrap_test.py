#!/usr/bin/env python3
"""Exercise pinned vendor patching and material-tool refresh without networking."""
import hashlib
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
GIT = shutil.which("git")
BASH = shutil.which("bash")


def replace_pin(script, name, value):
    script, count = re.subn(rf'^{name}="[^"]*"$', lambda _: f'{name}="{value}"',
                            script, count=1, flags=re.MULTILINE)
    if count != 1:
        raise AssertionError(f"Missing bootstrap pin: {name}")
    return script


@unittest.skipUnless(GIT and BASH, "Git and Bash are required for dependency bootstrap")
class DependencyBootstrapTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="heaven dependency pins ")
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.root = Path(cls.temporary.name)
        cls.environment = os.environ.copy()
        for name in ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_CONFIG_COUNT"):
            cls.environment.pop(name, None)
        cls.environment.update({
            "GIT_CONFIG_NOSYSTEM": "1", "GIT_CONFIG_GLOBAL": os.devnull,
            "GIT_AUTHOR_NAME": "Fixture", "GIT_AUTHOR_EMAIL": "fixture@example.invalid",
            "GIT_COMMITTER_NAME": "Fixture", "GIT_COMMITTER_EMAIL": "fixture@example.invalid",
            "GIT_AUTHOR_DATE": "2001-01-01T00:00:00+0000",
            "GIT_COMMITTER_DATE": "2001-01-01T00:00:00+0000",
            "LC_ALL": "C",
        })
        cls.seed = cls.root / "public upstream"
        cls.seed.mkdir()
        cls.git(cls.seed, "init", "--quiet")
        (cls.seed / "base.txt").write_text("upstream\n", newline="\n")
        cls.git(cls.seed, "add", "base.txt")
        cls.git(cls.seed, "commit", "--quiet", "-m", "Public upstream fixture")
        cls.base = cls.git(cls.seed, "rev-parse", "HEAD")
        (cls.seed / "base.txt").write_text("upstream with rendering extension\n", newline="\n")
        (cls.seed / "added.txt").write_text("new extension\n", newline="\n")
        cls.git(cls.seed, "add", "base.txt", "added.txt")
        cls.patch = subprocess.check_output(
            [GIT, "-C", str(cls.seed), "diff", "--cached", "--binary", "--full-index", cls.base],
            env=cls.environment)
        if b"\r\n" in cls.patch:
            raise AssertionError("Bootstrap fixture patch must retain LF bytes")
        cls.tree = cls.git(cls.seed, "write-tree")
        cls.git(cls.seed, "commit", "--quiet", "-m", "Local rendering extension")
        cls.counter = 0

    @classmethod
    def git(cls, directory, *arguments):
        result = subprocess.run([GIT, "-C", str(directory), *arguments], env=cls.environment,
                                text=True, capture_output=True, check=True)
        return result.stdout.strip()

    def project(self):
        type(self).counter += 1
        project = self.root / f"source archive {self.counter}"
        (project / "scripts").mkdir(parents=True)
        (project / "patches/filament").mkdir(parents=True)
        (project / "patches/filament/heavenbench.patch").write_bytes(self.patch)
        script = (ROOT / "scripts/fetch_dependencies.sh").read_text()
        pins = {"FILAMENT_URL": self.seed.as_posix(), "FILAMENT_UPSTREAM_REV": self.base,
                "FILAMENT_PATCH_SHA256": hashlib.sha256(self.patch).hexdigest(),
                "FILAMENT_EXPECTED_TREE": self.tree}
        for name in ("JOLT_REV", "RAPIDCHECK_REV", "LIBZIP_REV", "POCKETFFT_REV", "BCDEC_REV"):
            pins[name] = self.base
        for name, value in pins.items():
            script = replace_pin(script, name, value)
        # Every dependency comes from the tiny local fixture, never a network.
        script = re.sub(r'https://[^"\s]+\.git', lambda _: self.seed.as_posix(), script)
        (project / "scripts/fetch_dependencies.sh").write_text(script, newline="\n")
        return project

    def run_bootstrap(self, project, environment=None):
        self.assertNotIn(b"\r\n", (project / "scripts/fetch_dependencies.sh").read_bytes())
        return subprocess.run([BASH, (project / "scripts/fetch_dependencies.sh").as_posix()],
                              cwd=project, env=environment or self.environment, text=True,
                              capture_output=True, check=False, timeout=30)

    def fetch_fault(self, project, dependency, failures, error, status=128):
        binary = project / "fault-bin"
        binary.mkdir()
        count = project / "fetch-count.txt"
        attempts = project / "fetch-attempts.txt"
        sleeps = project / "fetch-sleeps.txt"
        wrapper = binary / "git"
        wrapper.write_text(
            '#!/usr/bin/env bash\nset -euo pipefail\n'
            f'if [[ "${{1:-}}" == -C && "${{3:-}}" == fetch && "$2" == */{dependency} ]]; then\n'
            f'  count=0; if [[ -f {shlex.quote(count.as_posix())} ]]; then read -r count < {shlex.quote(count.as_posix())}; fi\n'
            '  count=$((count + 1))\n'
            f'  printf "%s\\n" "$count" > {shlex.quote(count.as_posix())}\n'
            f'  printf "%s\\n" "$@" >> {shlex.quote(attempts.as_posix())}\n'
            f'  if [[ "$count" -le {failures} ]]; then\n'
            f'    printf "%s\\n" {shlex.quote(error)} >&2\n    exit {status}\n  fi\nfi\n'
            f'exec {shlex.quote(Path(GIT).as_posix())} "$@"\n', newline="\n")
        wrapper.chmod(0o755)
        sleep = binary / "sleep"
        sleep.write_text('#!/bin/sh\n' +
                         f'printf "%s\\n" "$@" >> {shlex.quote(sleeps.as_posix())}\n', newline="\n")
        sleep.chmod(0o755)
        environment = {**self.environment,
                       "PATH": str(binary) + os.pathsep + self.environment["PATH"]}
        return environment, count, attempts, sleeps

    def require_success(self, result):
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def base_checkout(self, project):
        directory = project / "lib/filament"
        directory.mkdir(parents=True)
        self.git(directory, "init", "--quiet")
        self.git(directory, "fetch", "--quiet", "--depth", "1", str(self.seed), self.base)
        self.git(directory, "checkout", "--quiet", "--detach", "FETCH_HEAD")
        return directory

    def assert_base_preserved(self, directory):
        self.assertEqual(self.git(directory, "rev-parse", "HEAD"), self.base)
        self.assertEqual(self.git(directory, "status", "--porcelain", "--untracked-files=no"), "")
        self.assertEqual((directory / "base.txt").read_text(), "upstream\n")

    def test_fresh_source_archive_and_repeat_are_deterministic(self):
        first = self.project()
        second = self.project()
        self.assertFalse((first / ".git").exists())
        self.require_success(self.run_bootstrap(first))
        configured = {**self.environment, "GIT_CONFIG_COUNT": "2",
                      "GIT_CONFIG_KEY_0": "i18n.commitEncoding", "GIT_CONFIG_VALUE_0": "ISO-8859-1",
                      "GIT_CONFIG_KEY_1": "commit.gpgsign", "GIT_CONFIG_VALUE_1": "true"}
        self.require_success(self.run_bootstrap(second, configured))
        directory = first / "lib/filament"
        revision = self.git(directory, "rev-parse", "HEAD")
        self.assertEqual(revision, self.git(second / "lib/filament", "rev-parse", "HEAD"))
        self.assertEqual(self.git(directory, "rev-parse", "HEAD^{tree}"), self.tree)
        self.assertEqual(self.git(directory, "rev-parse", "HEAD^"), self.base)
        self.assertEqual(self.git(directory, "status", "--porcelain"), "")
        # Unavailable remotes must not matter once all exact pins are present.
        script = first / "scripts/fetch_dependencies.sh"
        script.write_text(script.read_text().replace(self.seed.as_posix(), "/absent/upstream"), newline="\n")
        self.require_success(self.run_bootstrap(first))
        self.assertEqual(self.git(directory, "rev-parse", "HEAD"), revision)

    def test_transient_fetch_recovers_with_identical_pins(self):
        for dependency, error in (
                ("filament", "fatal: Could not resolve host: fixture.invalid"),
                ("pocketfft", "fatal: Failed to connect to fixture.invalid:443 after 21223 ms: Could not connect to server")):
            with self.subTest(dependency=dependency):
                project = self.project()
                environment, count, attempts, sleeps = self.fetch_fault(project, dependency, 1, error)
                result = self.run_bootstrap(project, environment)
                self.require_success(result)
                self.assertIn(error, result.stderr)
                self.assertEqual(count.read_text().strip(), "2")
                self.assertEqual(sleeps.read_text().splitlines(), ["2"])
                arguments = attempts.read_text().splitlines()
                self.assertEqual(len(arguments), 16)
                self.assertEqual(arguments[:8], arguments[8:])
                self.assertEqual(arguments[0], "-C")
                self.assertTrue(arguments[1].replace("\\", "/").endswith("/lib/" + dependency))
                self.assertEqual(arguments[2:8], ["fetch", "--quiet", "--depth", "1",
                                                self.seed.as_posix(), self.base])
                expected = self.tree if dependency == "filament" else self.base
                expression = "HEAD^{tree}" if dependency == "filament" else "HEAD"
                self.assertEqual(self.git(project / "lib" / dependency, "rev-parse", expression), expected)

    def test_permanent_transport_failure_is_bounded_and_resumable(self):
        project = self.project()
        error = "fatal: The requested URL returned error: 503\nremote: retained diagnostic context"
        environment, count, attempts, sleeps = self.fetch_fault(project, "pocketfft", 3, error, status=73)
        result = self.run_bootstrap(project, environment)
        self.assertEqual(result.returncode, 73, result.stdout + result.stderr)
        self.assertEqual(result.stderr.count(error), 3)
        self.assertEqual(count.read_text().strip(), "3")
        self.assertEqual(sleeps.read_text().splitlines(), ["2", "4"])
        self.assertEqual(attempts.read_text().splitlines().count(self.base), 3)
        directory = project / "lib/pocketfft"
        self.assertEqual(self.git(directory, "status", "--porcelain", "--untracked-files=all"), "")
        self.assertFalse((project / "lib/bcdec").exists())
        self.assertFalse((directory / ".git/index.lock").exists())
        self.require_success(self.run_bootstrap(project))
        self.assertEqual(self.git(directory, "rev-parse", "HEAD"), self.base)

    def test_unavailable_revision_is_not_retried(self):
        project = self.project()
        error = f"fatal: couldn't find remote ref {self.base}"
        environment, count, _, sleeps = self.fetch_fault(project, "pocketfft", 3, error)
        result = self.run_bootstrap(project, environment)
        self.assertEqual(result.returncode, 128, result.stdout + result.stderr)
        self.assertIn(error, result.stderr)
        self.assertEqual(count.read_text().strip(), "1")
        self.assertFalse(sleeps.exists())

    def test_existing_source_pin_mismatch_cannot_retry_into_acceptance(self):
        project = self.project()
        self.require_success(self.run_bootstrap(project))
        directory = project / "lib/pocketfft"
        (directory / "local.txt").write_text("keep local commit\n")
        self.git(directory, "add", "local.txt")
        self.git(directory, "commit", "--quiet", "-m", "Different source pin")
        before = self.git(directory, "rev-parse", "HEAD")
        environment, count, _, sleeps = self.fetch_fault(
            project, "pocketfft", 0, "fatal: Failed to connect")
        result = self.run_bootstrap(project, environment)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("expected", result.stderr)
        self.assertFalse(count.exists())
        self.assertFalse(sleeps.exists())
        self.assertEqual(self.git(directory, "rev-parse", "HEAD"), before)
        self.assertEqual((directory / "local.txt").read_text(), "keep local commit\n")

    def test_existing_rebased_commit_is_accepted_by_exact_tree(self):
        project = self.project()
        self.require_success(self.run_bootstrap(project))
        directory = project / "lib/filament"
        alternate = self.git(directory, "commit-tree", self.tree, "-p", self.base,
                             "-m", "Rebased manually with different metadata")
        self.git(directory, "checkout", "--quiet", "--detach", alternate)
        self.require_success(self.run_bootstrap(project))
        self.assertEqual(self.git(directory, "rev-parse", "HEAD"), alternate)

    def test_clean_upstream_is_patched(self):
        project = self.project()
        directory = self.base_checkout(project)
        self.require_success(self.run_bootstrap(project))
        self.assertEqual(self.git(directory, "rev-parse", "HEAD^{tree}"), self.tree)

    @unittest.skipUnless(os.name == "posix" and shutil.which("shasum"),
                         "macOS-compatible shasum and POSIX links are required")
    def test_bootstrap_without_gnu_sha256sum(self):
        project = self.project()
        binary = project / "portable-bin"
        binary.mkdir()
        for name in ("bash", "dirname", "git", "mktemp", "rm", "shasum"):
            executable = shutil.which(name)
            self.assertIsNotNone(executable, name)
            (binary / name).symlink_to(executable)
        environment = {**self.environment, "PATH": str(binary)}
        self.require_success(self.run_bootstrap(project, environment))
        directory = project / "lib/filament"
        self.assertEqual(self.git(directory, "rev-parse", "HEAD^{tree}"), self.tree)
        patch = project / "patches/filament/heavenbench.patch"
        patch.write_bytes(self.patch + b"\nchanged\n")
        result = self.run_bootstrap(project, environment)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("SHA-256", result.stderr)
        self.assertEqual(self.git(directory, "rev-parse", "HEAD^{tree}"), self.tree)

    def test_checksum_mismatch_preserves_checkout(self):
        project = self.project()
        directory = self.base_checkout(project)
        patch = project / "patches/filament/heavenbench.patch"
        patch.write_bytes(self.patch + b"\nmodified without changing pin\n")
        result = self.run_bootstrap(project)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("SHA-256", result.stderr)
        self.assert_base_preserved(directory)

    def test_wrong_expected_tree_preserves_real_index_and_worktree(self):
        project = self.project()
        directory = self.base_checkout(project)
        script = project / "scripts/fetch_dependencies.sh"
        script.write_text(replace_pin(script.read_text(), "FILAMENT_EXPECTED_TREE", "0" * 40), newline="\n")
        result = self.run_bootstrap(project)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("patch produced source tree", result.stderr)
        self.assert_base_preserved(directory)

    def test_unapplicable_patch_preserves_real_index_and_worktree(self):
        project = self.project()
        directory = self.base_checkout(project)
        patch = self.patch.replace(b"-upstream\n", b"-different upstream\n")
        self.assertNotEqual(patch, self.patch, "Unapplicable-patch negative control must mutate the patch")
        (project / "patches/filament/heavenbench.patch").write_bytes(patch)
        script = project / "scripts/fetch_dependencies.sh"
        script.write_text(replace_pin(script.read_text(), "FILAMENT_PATCH_SHA256",
                                      hashlib.sha256(patch).hexdigest()), newline="\n")
        self.assertNotEqual(self.run_bootstrap(project).returncode, 0)
        self.assert_base_preserved(directory)

    def test_local_edits_in_either_index_or_worktree_are_preserved(self):
        for kind in ("unstaged", "staged", "staged then undone in worktree"):
            with self.subTest(kind=kind):
                project = self.project()
                directory = self.base_checkout(project)
                source = directory / "base.txt"
                source.write_text("user changes\n")
                if kind != "unstaged":
                    self.git(directory, "add", "base.txt")
                if kind == "staged then undone in worktree":
                    source.write_text("upstream\n")
                index = self.git(directory, "write-tree")
                content = source.read_bytes()
                result = self.run_bootstrap(project)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("tracked changes", result.stderr)
                self.assertEqual(self.git(directory, "write-tree"), index)
                self.assertEqual(source.read_bytes(), content)

    def test_wrong_commit_is_not_reset(self):
        project = self.project()
        directory = self.base_checkout(project)
        (directory / "user.txt").write_text("unrelated local change\n")
        self.git(directory, "add", "user.txt")
        self.git(directory, "commit", "--quiet", "-m", "User work")
        revision = self.git(directory, "rev-parse", "HEAD")
        self.assertNotEqual(self.run_bootstrap(project).returncode, 0)
        self.assertEqual(self.git(directory, "rev-parse", "HEAD"), revision)

    def test_interrupted_empty_initialization_recovers(self):
        project = self.project()
        directory = project / "lib/filament"
        directory.mkdir(parents=True)
        self.git(directory, "init", "--quiet")
        self.require_success(self.run_bootstrap(project))
        self.assertEqual(self.git(directory, "rev-parse", "HEAD^{tree}"), self.tree)

    def test_interrupted_initialization_with_local_files_is_preserved(self):
        project = self.project()
        directory = project / "lib/filament"
        directory.mkdir(parents=True)
        self.git(directory, "init", "--quiet")
        (directory / ".git/info/exclude").write_text("keep.txt\n")
        (directory / "keep.txt").write_text("ignored but valuable\n")
        result = self.run_bootstrap(project)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("contains local files", result.stderr)
        self.assertEqual((directory / "keep.txt").read_text(), "ignored but valuable\n")

    def test_untracked_checkout_collision_is_preserved(self):
        project = self.project()
        directory = self.base_checkout(project)
        (directory / "added.txt").write_text("user untracked data\n")
        self.assertNotEqual(self.run_bootstrap(project).returncode, 0)
        self.assert_base_preserved(directory)
        self.assertEqual((directory / "added.txt").read_text(), "user untracked data\n")

    def test_ignored_checkout_collision_is_preserved_and_can_recover(self):
        project = self.project()
        directory = self.base_checkout(project)
        (directory / ".git/info/exclude").write_text("added.txt\n")
        collision = directory / "added.txt"
        collision.write_text("ignored local user data\n")
        # A clean status does not mean the ignored path is disposable. The
        # patch introduces this same filename, so checkout must refuse it.
        self.assertEqual(self.git(directory, "status", "--porcelain"), "")
        self.assertNotEqual(self.run_bootstrap(project).returncode, 0)
        self.assert_base_preserved(directory)
        self.assertEqual(collision.read_text(), "ignored local user data\n")
        saved = project / "saved-added.txt"
        collision.rename(saved)
        self.require_success(self.run_bootstrap(project))
        self.assertEqual(self.git(directory, "rev-parse", "HEAD^{tree}"), self.tree)
        self.assertEqual(collision.read_text(), "new extension\n")
        self.assertEqual(saved.read_text(), "ignored local user data\n")
        self.require_success(self.run_bootstrap(project))

    def test_other_dependency_staged_changes_are_preserved(self):
        project = self.project()
        self.require_success(self.run_bootstrap(project))
        directory = project / "lib/JoltPhysics"
        (directory / "base.txt").write_text("user staged change\n")
        self.git(directory, "add", "base.txt")
        (directory / "base.txt").write_text("upstream\n")
        index = self.git(directory, "write-tree")
        self.assertNotEqual(self.run_bootstrap(project).returncode, 0)
        self.assertEqual(self.git(directory, "write-tree"), index)

    def test_pinned_patch_checkout_keeps_exact_bytes_with_autocrlf(self):
        project = self.project()
        relative = "patches/filament/heavenbench.patch"
        original = (ROOT / relative).read_bytes()
        (project / relative).write_bytes(original)
        shutil.copyfile(ROOT / ".gitattributes", project / ".gitattributes")
        self.git(project, "init", "--quiet")
        self.git(project, "config", "core.autocrlf", "true")
        self.git(project, "add", ".gitattributes", relative)
        self.git(project, "commit", "--quiet", "-m", "Pinned patch checkout fixture")
        (project / relative).unlink()
        self.git(project, "checkout", "HEAD", "--", relative)
        checked_out = (project / relative).read_bytes()
        self.assertEqual(hashlib.sha256(checked_out).hexdigest(), hashlib.sha256(original).hexdigest())
        self.assertEqual(checked_out, original)

    def test_bootstrap_checkout_runs_with_autocrlf(self):
        project = self.project()
        relative = "scripts/fetch_dependencies.sh"
        shutil.copyfile(ROOT / ".gitattributes", project / ".gitattributes")
        self.git(project, "init", "--quiet")
        self.git(project, "config", "core.autocrlf", "true")
        self.git(project, "add", ".gitattributes", relative,
                 "patches/filament/heavenbench.patch")
        self.git(project, "commit", "--quiet", "-m", "Bash checkout fixture")
        (project / relative).unlink()
        self.git(project, "checkout", "HEAD", "--", relative)
        self.assertNotIn(b"\r\n", (project / relative).read_bytes())
        self.require_success(self.run_bootstrap(project))


@unittest.skipUnless(sys.platform.startswith("linux") and BASH, "Linux material build helper")
class MaterialCompilerRefreshTest(unittest.TestCase):
    def check_material_action(self, option, unchanged=False):
        with tempfile.TemporaryDirectory(prefix="heaven material refresh ") as temporary:
            root = Path(temporary)
            scripts = root / "scripts"
            scripts.mkdir()
            for name in ("build.sh", "build_paths.sh"):
                shutil.copyfile(ROOT / "scripts" / name, scripts / name)
            materials = root / "benchmark/materials"
            materials.mkdir(parents=True)
            source = materials / "heaven_lit.mat"
            source.write_text("fixture material\n")
            package = materials / "heaven_lit.filamat"
            package.write_text("previous known-good package\n")
            os.utime(source, (10 if unchanged else 30, 10 if unchanged else 30))
            os.utime(package, (20, 20))
            build = root / "build"
            matc = build / "filament/tools/matc/matc"
            matc.parent.mkdir(parents=True)
            matc.write_text("#!/bin/sh\necho stale-matc >> \"$TEST_EVENTS\"\nexit 99\n")
            matc.chmod(0o755)
            fresh = root / "fresh-matc"
            fresh.write_text(
                '#!/bin/sh\necho fresh-matc >> "$TEST_EVENTS"\n'
                'while [ "$#" -gt 0 ]; do\n'
                '  if [ "$1" = -o ]; then printf compiled > "$2"; exit 0; fi\n'
                '  shift\ndone\nexit 1\n')
            fresh.chmod(0o755)
            binary = root / "bin"
            binary.mkdir()
            cmake = binary / "cmake"
            cmake.write_text(
                '#!/bin/sh\nprintf "cmake %s\\n" "$*" >> "$TEST_EVENTS"\n'
                'if [ "$1" = --build ] && [ "$4" = matc ]; then\n'
                '  cp "$TEST_FRESH_MATC" "$TEST_MATC"\nfi\n')
            cmake.chmod(0o755)
            for name in ("clang", "clang++", "ninja"):
                path = binary / name
                path.write_text("#!/bin/sh\nexit 0\n")
                path.chmod(0o755)
            (scripts / "gen_materials_data.py").write_text(
                'import os\nwith open(os.environ["TEST_EVENTS"], "a") as f:\n'
                '    f.write("regenerated\\n")\n')
            events = root / "events.log"
            environment = os.environ.copy()
            environment.update({"PATH": str(binary) + os.pathsep + environment["PATH"],
                                "HEAVEN_COMPILER": "clang", "HEAVEN_C_COMPILER": str(binary / "clang"),
                                "HEAVEN_CXX_COMPILER": str(binary / "clang++"),
                                "TEST_EVENTS": str(events), "TEST_FRESH_MATC": str(fresh),
                                "TEST_MATC": str(matc)})
            result = subprocess.run([BASH, str(scripts / "build.sh"), option,
                                     "--build-dir", str(build), "--jobs", "2", "--target", "fixture"],
                                    env=environment, text=True, capture_output=True, check=False, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            lines = events.read_text().splitlines()
            refresh = next(i for i, line in enumerate(lines) if "--target matc --parallel 2" in line)
            self.assertNotIn("stale-matc", lines)
            if unchanged and option == "--materials":
                self.assertNotIn("fresh-matc", lines)
                self.assertNotIn("regenerated", lines)
                self.assertEqual(package.read_text(), "previous known-good package\n")
            else:
                self.assertLess(refresh, lines.index("fresh-matc"))
                self.assertLess(lines.index("fresh-matc"), lines.index("regenerated"))
                self.assertEqual(package.read_text(), "compiled")

    def test_changed_material_uses_refreshed_compiler(self):
        self.check_material_action("--materials")

    def test_unchanged_material_is_preserved(self):
        self.check_material_action("--materials", unchanged=True)

    def test_explicit_full_recompile_uses_refreshed_compiler(self):
        self.check_material_action("--materials-all", unchanged=True)


if __name__ == "__main__":
    unittest.main()
