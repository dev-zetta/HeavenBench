#!/usr/bin/env python3
"""Verify Docker orchestration and exact source exports without building images."""

import hashlib
import json
import os
from pathlib import Path
import posixpath
import re
import shlex
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
BASH = shutil.which("bash")


@unittest.skipUnless(os.name == "posix" and BASH, "Docker wrapper fixtures require a POSIX Bash host")
class DockerBuildTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="heaven docker contract ")
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name).resolve()
        self.source = self.directory / "source with spaces"
        (self.source / "scripts").mkdir(parents=True)
        (self.source / "docker").mkdir()
        self.wrapper = self.source / "scripts/build_docker.sh"
        shutil.copyfile(ROOT / "scripts/build_docker.sh", self.wrapper)
        (self.source / "docker/Dockerfile").write_text("FROM scratch\n")
        self.executables = self.directory / "executables"
        self.executables.mkdir()
        self.calls = self.directory / "docker-calls.jsonl"
        docker = self.executables / "docker"
        docker.write_text(
            f"#!{sys.executable}\n"
            "import json, os, sys\n"
            "with open(os.environ['FIXTURE_DOCKER_CALLS'], 'a') as output:\n"
            "    output.write(json.dumps(sys.argv[1:]) + '\\n')\n"
            "print('fixture docker diagnostic', flush=True)\n"
            "sys.exit(int(os.environ.get('FIXTURE_DOCKER_EXIT', '0')))\n")
        docker.chmod(0o755)
        self.environment = {**os.environ,
                            "PATH": str(self.executables) + os.pathsep + os.environ.get("PATH", ""),
                            "FIXTURE_DOCKER_CALLS": str(self.calls)}
        self.output = self.directory / "release output"

    def run_wrapper(self, *arguments, environment=None):
        return subprocess.run([BASH, str(self.wrapper), *map(str, arguments)],
                              cwd=self.directory, env=environment or self.environment,
                              text=True, capture_output=True, check=False, timeout=15)

    def invocations(self):
        return [json.loads(line) for line in self.calls.read_text().splitlines()] if self.calls.exists() else []

    def require_success(self, result):
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def argument(self, command, option):
        self.assertIn(option, command)
        return command[command.index(option) + 1]

    @staticmethod
    def build_arguments(command):
        return dict(command[index + 1].split("=", 1) for index, value in enumerate(command)
                    if value == "--build-arg")

    def test_linux_defaults_use_local_exporter_and_isolated_context(self):
        self.require_success(self.run_wrapper("linux"))
        [command] = self.invocations()
        self.assertEqual(command[:2], ["buildx", "build"])
        self.assertEqual(self.argument(command, "--platform"), "linux/amd64")
        self.assertEqual(self.argument(command, "--target"), "linux-artifacts")
        self.assertEqual(self.argument(command, "--file"), str(self.source / "docker/Dockerfile"))
        self.assertEqual(command[-1], str(self.source))
        self.assertEqual(self.argument(command, "--output"),
                         "type=local,dest=" + str(self.source / "build/release/linux-portable"))
        args = self.build_arguments(command)
        self.assertEqual(args["CPU_ARCH"], "portable")
        self.assertEqual(args["JOBS"], "2")
        self.assertEqual(args["RUN_ANALYSIS"], "1")
        self.assertEqual(args["CACHE_SCOPE"], "default")
        for forbidden in ("run", "--privileged", "--volume", "-v", "--mount", "--user", "--network=host"):
            self.assertNotIn(forbidden, command)
        self.assertNotIn("--no-cache", command)

    def test_linux_architectures_select_separate_outputs(self):
        for architecture in ("portable", "zen1", "zen2", "zen3", "zen4", "zen5"):
            with self.subTest(architecture=architecture):
                self.require_success(self.run_wrapper("linux", "--cpu-arch", architecture,
                                                      "--jobs", "3", "--skip-analysis"))
                command = self.invocations()[-1]
                args = self.build_arguments(command)
                self.assertEqual(args["CPU_ARCH"], architecture)
                self.assertEqual(args["JOBS"], "3")
                self.assertEqual(args["RUN_ANALYSIS"], "0")
                self.assertEqual(self.argument(command, "--output"),
                                 "type=local,dest=" + str(self.source / f"build/release/linux-{architecture}"))

    def test_android_default_and_explicit_abi(self):
        for arguments, abi in (([], "arm64-v8a"), (["--abi", "x86_64"], "x86_64")):
            with self.subTest(abi=abi):
                self.require_success(self.run_wrapper("android", *arguments))
                command = self.invocations()[-1]
                self.assertEqual(self.argument(command, "--target"), "android-artifacts")
                self.assertEqual(self.build_arguments(command)["ANDROID_ABI"], abi)
                self.assertEqual(self.argument(command, "--output"),
                                 "type=local,dest=" + str(self.source / f"build/release/android-{abi}"))

    def test_experimental_fsr_uses_separate_artifacts_and_keeps_analysis(self):
        self.require_success(self.run_wrapper("linux", "--experimental-fsr3"))
        [command] = self.invocations()
        self.assertEqual(self.argument(command, "--target"), "linux-fsr3-artifacts")
        self.assertEqual(self.argument(command, "--output"),
                         "type=local,dest=" + str(self.source / "build/release/linux-experimental-fsr3"))
        self.assertEqual(self.build_arguments(command)["RUN_ANALYSIS"], "1")
        self.assertEqual(self.build_arguments(command)["CPU_ARCH"], "portable")

    def test_experimental_fsr_rejects_android_and_nonportable(self):
        for arguments in (("android", "--experimental-fsr3"), ("linux", "--experimental-fsr3", "--cpu-arch", "zen3")):
            self.assertNotEqual(self.run_wrapper(*arguments).returncode, 0)
        self.assertEqual(self.invocations(), [])

    def test_output_spaces_and_shell_metacharacters_remain_literal(self):
        name = "packages $(touch EXPANDED) `touch EVALUATED` $HOME; output"
        self.require_success(self.run_wrapper("linux", "--output-dir", name))
        [command] = self.invocations()
        self.assertEqual(self.argument(command, "--output"),
                         "type=local,dest=" + str(self.directory / name))
        self.assertFalse((self.directory / "EXPANDED").exists())
        self.assertFalse((self.directory / "EVALUATED").exists())

    def test_clean_changes_cache_scope_without_deleting_existing_artifacts(self):
        self.output.mkdir()
        marker = self.output / "previous-release.zip"
        marker.write_bytes(b"retain prior output")
        scopes = set()
        for _ in range(2):
            self.require_success(self.run_wrapper("linux", "--clean", "--output-dir", self.output))
            command = self.invocations()[-1]
            self.assertIn("--no-cache", command)
            scopes.add(self.build_arguments(command)["CACHE_SCOPE"])
            self.assertEqual(marker.read_bytes(), b"retain prior output")
        self.assertEqual(len(scopes), 2)
        self.assertNotIn("default", scopes)
        self.assertEqual(len(self.invocations()), 2, "Clean must not issue extra Docker prune commands")

    def test_docker_failure_propagates_through_logging(self):
        result = self.run_wrapper("linux", "--output-dir", self.output,
                                  environment={**self.environment, "FIXTURE_DOCKER_EXIT": "37"})
        self.assertEqual(result.returncode, 37, result.stdout + result.stderr)
        self.assertIn("fixture docker diagnostic", (self.output / "logs/docker-build.log").read_text())

    def test_invalid_options_fail_before_docker(self):
        cases = [[], ["windows"], ["linux", "--unknown"], ["linux", "--jobs"],
                 ["linux", "--jobs", "0"], ["linux", "--jobs", "-1"],
                 ["linux", "--jobs", "1;touch escaped"], ["linux", "--cpu-arch", "native"],
                 ["linux", "--cpu-arch", "zen6"], ["linux", "--abi", "x86_64"],
                 ["android", "--abi", "armeabi-v7a"], ["android", "--cpu-arch", "portable"],
                 ["android", "--skip-analysis"], ["linux", "--output-dir", ""],
                 ["linux", "--output-dir", "output,uid=0"],
                 ["linux", "--output-dir", "output\nnewline"],
                 ["linux", "--output-dir", "output\rcarriage"]]
        for arguments in cases:
            with self.subTest(arguments=arguments):
                self.assertNotEqual(self.run_wrapper(*arguments).returncode, 0)
        self.assertEqual(self.invocations(), [])

    def test_source_directories_cannot_be_used_as_output(self):
        for directory in (self.source, self.source / "scripts", self.source / "benchmark/output"):
            with self.subTest(directory=directory):
                result = self.run_wrapper("linux", "--output-dir", directory)
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.invocations(), [])

    def test_help_does_not_contact_docker(self):
        for arguments in (("--help",), ("linux", "--help"), ("android", "--help")):
            self.require_success(self.run_wrapper(*arguments))
        self.assertEqual(self.invocations(), [])


@unittest.skipUnless(os.name == "posix" and BASH, "Linux pipeline fixtures require Bash")
class LinuxPipelineTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="heaven linux pipeline ")
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        scripts = self.directory / "scripts"
        scripts.mkdir()
        for name in ("ci_linux.sh", "build_paths.sh"):
            shutil.copyfile(ROOT / "scripts" / name, scripts / name)
        self.calls = self.directory / "calls.jsonl"
        executables = self.directory / "executables"
        executables.mkdir()
        self.build_receipts = {
            "CMakeCache.txt": b"HEAVEN_ENABLE_FSR3:BOOL=ON\n",
            "src/rhi/frame_generation/fsr31_shader_blobs.json": b'{"fixture_shader_blobs": [1, 2, 3]}\n',
        }
        command = (f"#!{sys.executable}\n"
                   "import json, os, pathlib, re, sys\n"
                   "name = pathlib.Path(sys.argv[0]).name\n"
                   "args = sys.argv[1:]\n"
                   "with open(os.environ['FIXTURE_LINUX_CALLS'], 'a') as output:\n"
                   "    output.write(json.dumps({'name': name, 'arguments': args}) + '\\n')\n"
                   "if name == 'ctest' and '--test-dir' in args and pathlib.Path(args[args.index('--test-dir') + 1]).name == 'renderer':\n"
                   "    failing = os.environ.get('FIXTURE_CTEST_FAILURE')\n"
                   "    selected = bool(failing)\n"
                   "    for option in ('-R', '--tests-regex'):\n"
                   "        if option in args and failing and not re.search(args[args.index(option) + 1], failing):\n"
                   "            selected = False\n"
                   "    for option in ('-E', '--exclude-regex'):\n"
                   "        if option in args and failing and re.search(args[args.index(option) + 1], failing):\n"
                   "            selected = False\n"
                   "    if selected:\n"
                   "        sys.exit(37)\n"
                   "if name == 'build.sh' and '-DHEAVEN_ENABLE_FSR3=ON' in args:\n"
                   "    directory = pathlib.Path(os.environ['HEAVEN_BUILD_ROOT']) / 'renderer'\n"
                   f"    receipts = {self.build_receipts!r}\n"
                   "    for relative, data in receipts.items():\n"
                   "        if relative == os.environ.get('FIXTURE_MISSING_BUILD_RECEIPT'):\n"
                   "            continue\n"
                   "        path = directory / relative; path.parent.mkdir(parents=True, exist_ok=True)\n"
                   "        path.write_bytes(data)\n"
                   "if name == 'package_appimage.sh':\n"
                   "    directory = pathlib.Path(args[args.index('--output-dir') + 1])\n"
                   "    architecture = args[args.index('--cpu-arch') + 1]\n"
                   "    suffix = '-experimental-fsr3-123456789abc' if '--experimental-fsr3' in args else ''\n"
                   "    (directory / f'fixture-{architecture}{suffix}.AppImage').write_bytes(b'fixture')\n"
                   "if name == 'fetch_fsr31.py':\n"
                   "    directory = pathlib.Path(args[args.index('--output') + 1]); directory.mkdir(parents=True, exist_ok=True)\n"
                   "    (directory / 'heavenbench-source.json').write_text('{}')\n")
        for directory, names in ((executables, ("cmake", "ctest", "glslangValidator", "spirv-val")),
                                 (scripts, ("fetch_dependencies.sh", "build_asset_tools.sh", "build.sh",
                                            "check_modern_cpp.sh", "package_appimage.sh", "verify_appimage.py", "fetch_fsr31.py"))):
            for name in names:
                path = directory / name
                path.write_text(command)
                path.chmod(0o755)
        self.environment = {**os.environ,
                            "PATH": str(executables) + os.pathsep + os.environ.get("PATH", ""),
                            "HEAVEN_BUILD_ROOT": str(self.directory / "build"),
                            "FIXTURE_LINUX_CALLS": str(self.calls)}

    def run_pipeline(self, architecture="portable", arguments=(), **settings):
        result = subprocess.run([BASH, str(self.directory / "scripts/ci_linux.sh"),
                                 "--cpu-arch", architecture, "--output-dir", str(self.directory / "artifacts"), *map(str, arguments)],
                                env={**self.environment, **settings}, text=True,
                                capture_output=True, check=False, timeout=15)
        return result, [json.loads(line) for line in self.calls.read_text().splitlines()]

    def renderer_test_calls(self, calls):
        return [call["arguments"] for call in calls if call["name"] == "ctest" and
                "--test-dir" in call["arguments"] and
                call["arguments"][call["arguments"].index("--test-dir") + 1] ==
                str(self.directory / "build/renderer")]

    def assert_host_and_smoke_gates(self, calls):
        host, smoke = self.renderer_test_calls(calls)
        for option in ("-R", "--tests-regex"):
            self.assertNotIn(option, host, "Host gate has a restrictive include regex")
        self.assertEqual(host.count("--exclude-regex"), 1)
        self.assertNotIn("-E", host)
        pattern = host[host.index("--exclude-regex") + 1]
        self.assertEqual(pattern, "^heaven_benchmark_smoke$", "Host gate must exclude only graphical smoke")
        for name in ("benchmark_tests", "profiler_trace_tests", "microprofile_analysis", "runtime_paths_cli",
                     "asset_packer_cli", "gtao_bent_normal_contract", "specular_occluded_moment_contract",
                     "surface_color_contract", "capture_camera_motion_tests", "frame_generation_history_tests",
                     "filament_ssr_variant_tests", "vulkan_pipeline_lifetime_tests", "dlss_contract_tests",
                     "dlss_runtime_bridge_tests", "fsr31_source_contract", "temporal_input_contract",
                     "filament_depth_program_lifetime", "filament_froxel_stream_lifetime",
                     "future_cpu_contract", "benchmark_smoke_cpu_contract"):
            self.assertIsNone(re.search(pattern, "heaven_" + name), name)
        self.assertIsNotNone(re.search(pattern, "heaven_benchmark_smoke"))
        self.assertEqual(smoke.count("--tests-regex"), 1)
        self.assertEqual(smoke[smoke.index("--tests-regex") + 1], "^heaven_benchmark_smoke$")
        for option in ("-R", "-E", "--exclude-regex"):
            self.assertNotIn(option, smoke)

    def test_portable_builds_and_selects_every_host_suite(self):
        result, calls = self.run_pipeline()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        [build] = [call["arguments"] for call in calls if call["name"] == "build.sh"]
        self.assertIn("heaven_profiler_trace_tests", build)
        self.assert_host_and_smoke_gates(calls)
        self.assertTrue(any(call["name"] == "verify_appimage.py" for call in calls))
        self.assertIn('-DHEAVEN_ENABLE_FSR3=OFF', build)
        self.assertIn('-DHEAVEN_ENABLE_DLSS=OFF', build)

    def test_experimental_pipeline_preserves_all_gates_and_labels_package(self):
        manifest = self.directory / 'source-manifest.json'; manifest.write_text('{}')
        result, calls = self.run_pipeline(arguments=('--experimental-fsr3', '--source-manifest', manifest))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        [build] = [call['arguments'] for call in calls if call['name'] == 'build.sh']
        self.assertIn('-DHEAVEN_ENABLE_FSR3=ON', build)
        self.assertIn('-DHEAVEN_ENABLE_DLSS=OFF', build)
        self.assertIn('-DHEAVEN_FSR31_NATIVE_TEST=OFF', build)
        self.assert_host_and_smoke_gates(calls)
        [package] = [call['arguments'] for call in calls if call['name'] == 'package_appimage.sh']
        self.assertIn('--experimental-fsr3', package)
        self.assertEqual(package[package.index('--source-manifest') + 1], str(manifest))
        self.assertTrue(any(call['name'] == 'check_modern_cpp.sh' for call in calls))
        self.assertTrue(any(call['name'] == 'verify_appimage.py' and 'experimental-fsr3' in call['arguments'][0] for call in calls))
        for relative, data in self.build_receipts.items():
            self.assertEqual((self.directory / 'artifacts/metadata' / Path(relative).name).read_bytes(), data)

    def test_experimental_missing_build_receipt_prevents_package(self):
        manifest = self.directory / 'source-manifest.json'; manifest.write_text('{}')
        for relative in self.build_receipts:
            with self.subTest(receipt=relative):
                shutil.rmtree(self.directory / 'build', ignore_errors=True)
                self.calls.unlink(missing_ok=True)
                result, calls = self.run_pipeline(arguments=('--experimental-fsr3', '--source-manifest', manifest),
                    FIXTURE_MISSING_BUILD_RECEIPT=relative)
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn(relative, result.stdout + result.stderr)
                self.assertEqual(self.renderer_test_calls(calls), [])
                self.assertFalse(any(call['name'] in ('check_modern_cpp.sh', 'package_appimage.sh') for call in calls))

    def test_experimental_host_failure_prevents_package(self):
        manifest = self.directory / 'source-manifest.json'; manifest.write_text('{}')
        result, calls = self.run_pipeline(arguments=('--experimental-fsr3', '--source-manifest', manifest),
            FIXTURE_CTEST_FAILURE='heaven_fsr31_view_retirement_test')
        self.assertEqual(result.returncode, 37, result.stdout + result.stderr)
        self.assertFalse(any(call['name'] == 'package_appimage.sh' for call in calls))

    def test_architecture_package_keeps_host_tests_on_portable_only(self):
        result, calls = self.run_pipeline("zen1")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse(any(call["name"] == "ctest" for call in calls))
        [build] = [call["arguments"] for call in calls if call["name"] == "build.sh"]
        self.assertNotIn("heaven_profiler_trace_tests", build)
        self.assertTrue(any(call["name"] == "package_appimage.sh" for call in calls))

    def test_new_host_failure_blocks_packaging(self):
        result, calls = self.run_pipeline(FIXTURE_CTEST_FAILURE="heaven_profiler_trace_tests")
        self.assertEqual(result.returncode, 37, result.stdout + result.stderr)
        self.assertEqual(len(self.renderer_test_calls(calls)), 1, "Host failure must precede graphical smoke")
        self.assertFalse(any(call["name"] == "package_appimage.sh" for call in calls))

    def test_future_host_failure_blocks_packaging(self):
        result, calls = self.run_pipeline(FIXTURE_CTEST_FAILURE="heaven_future_cpu_contract")
        self.assertEqual(result.returncode, 37, result.stdout + result.stderr)
        self.assertEqual(len(self.renderer_test_calls(calls)), 1, "Future host failure must precede graphical smoke")
        self.assertFalse(any(call["name"] == "package_appimage.sh" for call in calls))

    def test_graphical_smoke_failure_blocks_packaging_at_its_separate_gate(self):
        result, calls = self.run_pipeline(FIXTURE_CTEST_FAILURE="heaven_benchmark_smoke")
        self.assertEqual(result.returncode, 37, result.stdout + result.stderr)
        self.assert_host_and_smoke_gates(calls)
        self.assertFalse(any(call["name"] == "package_appimage.sh" for call in calls))

    def test_old_restrictive_host_gate_is_rejected(self):
        script = self.directory / "scripts/ci_linux.sh"
        current = "--exclude-regex '^heaven_benchmark_smoke$'"
        legacy = "--tests-regex '^heaven_(benchmark_tests|profiler_trace_tests)$'"
        source = script.read_text()
        self.assertEqual(source.count(current), 1)
        script.write_text(source.replace(current, legacy))
        result, calls = self.run_pipeline(FIXTURE_CTEST_FAILURE="heaven_future_cpu_contract")
        self.assertEqual(result.returncode, 0, "Legacy whitelist should demonstrate the missed future failure")
        self.assertTrue(any(call["name"] == "package_appimage.sh" for call in calls))
        with self.assertRaisesRegex(AssertionError, "restrictive include regex"):
            self.assert_host_and_smoke_gates(calls)


@unittest.skipUnless(os.name == "posix" and BASH, "Toolchain fixtures require Bash")
class ToolchainDownloadTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="heaven-toolchain-fetch-")
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.calls = self.directory / "calls.jsonl"
        executables = self.directory / "executables"
        executables.mkdir()
        # Exercise the actual Docker RUN chain with local command doubles.
        # curl's retry implementation/network availability remain CI evidence.
        command = (f"#!{sys.executable}\n"
                   "import json, os, pathlib, sys\n"
                   "name = pathlib.Path(sys.argv[0]).name\n"
                   "args = sys.argv[1:]\n"
                   "with open(os.environ['FIXTURE_FETCH_CALLS'], 'a') as output:\n"
                   "    output.write(json.dumps([name, *args]) + '\\n')\n"
                   "if name == 'curl':\n"
                   "    destination = pathlib.Path(args[args.index('--output') + 1])\n"
                   "    destination.write_bytes(b'partial failed download')\n"
                   "    if os.environ.get('FIXTURE_FETCH_FAILURE'):\n"
                   "        sys.exit(7)\n"
                   "    destination.write_bytes(b'invalid' if os.environ.get('FIXTURE_BAD_KEY') else b'complete key')\n"
                   "elif name == 'gpg':\n"
                   "    if pathlib.Path(args[-1]).read_bytes() != b'complete key':\n"
                   "        sys.exit(65)\n"
                   "    pathlib.Path(args[args.index('-o') + 1]).write_bytes(b'validated keyring')\n")
        for name in ("apt-get", "curl", "gpg"):
            path = executables / name
            path.write_text(command)
            path.chmod(0o755)
        self.environment = {**os.environ, "FIXTURE_FETCH_CALLS": str(self.calls),
                            "PATH": str(executables) + os.pathsep + os.environ.get("PATH", "")}
        self.key = self.directory / "download.key"
        self.keyring = self.directory / "keyring.gpg"
        self.repository = self.directory / "llvm.list"
        apt_lists = self.directory / "apt-lists"
        apt_lists.mkdir()
        self.command = (ROOT / "docker/Dockerfile").read_text().split("RUN ", 1)[1].split("\nENV ", 1)[0]
        self.command = self.command.replace("\\\n", " ")
        for original, replacement in (("/tmp/apt.llvm.org.gpg.key", self.key),
                                      ("/usr/share/keyrings/apt.llvm.org.gpg", self.keyring),
                                      ("/etc/apt/sources.list.d/llvm-21.list", self.repository),
                                      ("/var/lib/apt/lists", apt_lists)):
            self.command = self.command.replace(original, shlex.quote(str(replacement)))

    def run_fetch(self, **settings):
        result = subprocess.run([BASH, "-o", "pipefail", "-c", self.command],
                                env={**self.environment, **settings}, text=True,
                                capture_output=True, check=False, timeout=15)
        calls = [json.loads(line) for line in self.calls.read_text().splitlines()]
        return result, calls

    def test_complete_download_is_validated_before_repository_install(self):
        result, calls = self.run_fetch()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        [curl] = [call for call in calls if call[0] == "curl"]
        for option, value in (("--retry", "4"), ("--retry-delay", "3"), ("--retry-max-time", "180"),
                              ("--connect-timeout", "15"), ("--max-time", "30")):
            self.assertEqual(curl[curl.index(option) + 1], value)
        for option in ("--fail", "--location", "--retry-all-errors"):
            self.assertIn(option, curl)
        self.assertNotIn("--insecure", curl)
        self.assertEqual(curl[-1], "https://apt.llvm.org/llvm-snapshot.gpg.key")
        self.assertEqual([call[0] for call in calls], ["apt-get", "apt-get", "curl", "gpg", "apt-get", "apt-get"])
        for call in calls:
            if call[0] == "apt-get":
                self.assertEqual(call[1:3], ["-o", "Acquire::Retries=3"])
        self.assertEqual(self.keyring.read_bytes(), b"validated keyring")
        self.assertIn("llvm-toolchain-noble-21 main", self.repository.read_text())
        self.assertIn("signed-by=", self.repository.read_text())
        self.assertFalse(self.key.exists())

    def test_exhausted_download_stops_before_signature_or_repository_setup(self):
        result, calls = self.run_fetch(FIXTURE_FETCH_FAILURE="1")
        self.assertEqual(result.returncode, 7)
        self.assertEqual([call[0] for call in calls], ["apt-get", "apt-get", "curl"])
        self.assertFalse(self.keyring.exists())
        self.assertFalse(self.repository.exists())

    def test_invalid_download_fails_signature_validation(self):
        result, calls = self.run_fetch(FIXTURE_BAD_KEY="1")
        self.assertEqual(result.returncode, 65)
        self.assertEqual([call[0] for call in calls], ["apt-get", "apt-get", "curl", "gpg"])
        self.assertFalse(self.keyring.exists())
        self.assertFalse(self.repository.exists())


class SourceExportTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="heaven source export ")
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.source = self.directory / "filtered context"
        (self.source / "benchmark").mkdir(parents=True)
        (self.source / "benchmark/CMakeLists.txt").write_text(
            "project(HeavenBench VERSION 1.2.3 LANGUAGES CXX)\n")
        (self.source / "edited.cpp").write_bytes(b"initial content\n")
        (self.source / "edited.cpp").write_bytes(b"local edit used by this build\n")
        (self.source / "new untracked.cpp").write_bytes(b"new source without a Git commit\n")
        executable = self.source / "entrypoint.sh"
        executable.write_bytes(b"#!/bin/sh\nexit 0\n")
        executable.chmod(0o755)
        self.output = self.directory / "artifacts"
        self.environment = os.environ.copy()
        # Receipts must record tool output, but this fixture does not depend on
        # distro packages or invoke the real compiler/package database.
        executables = self.directory / "executables"
        executables.mkdir()
        for name in ("dpkg-query", "clang-21", "cmake"):
            command = executables / name
            command.write_text(f"#!{sys.executable}\nprint({name + ' fixture version'!r})\n")
            command.chmod(0o755)
        self.environment["PATH"] = str(executables) + os.pathsep + self.environment.get("PATH", "")

    def export(self, output=None):
        return subprocess.run([sys.executable, str(ROOT / "scripts/export_build_sources.py"),
                               "--source-root", str(self.source), "--output-dir", str(output or self.output),
                               "--platform", "linux-portable"], env=self.environment,
                              text=True, capture_output=True, check=False, timeout=15)

    def require_success(self, result):
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def archive(self, output=None):
        return (output or self.output) / "HeavenBench-1.2.3-source.tar.gz"

    def symlink(self, name, target):
        try:
            (self.source / name).symlink_to(target)
        except (OSError, NotImplementedError) as error:
            self.skipTest(f"Creating symlinks is unavailable: {error}")

    def test_source_archive_contains_exact_current_bytes_and_receipts(self):
        self.require_success(self.export())
        expected = {path.relative_to(self.source).as_posix(): path.read_bytes()
                    for path in self.source.rglob("*") if path.is_file()}
        prefix = "HeavenBench-1.2.3/"
        with tarfile.open(self.archive()) as archive:
            self.assertEqual(set(archive.getnames()), {prefix + name for name in expected})
            for name, content in expected.items():
                member = archive.getmember(prefix + name)
                self.assertEqual(archive.extractfile(member).read(), content)
                self.assertEqual((member.uid, member.gid, member.mtime), (0, 0, 0))
                expected_mode = 0o755 if (self.source / name).stat().st_mode & 0o111 else 0o644
                self.assertEqual(member.mode, expected_mode)
        digest = hashlib.sha256(self.archive().read_bytes()).hexdigest()
        self.assertEqual(self.archive().with_suffix(".gz.sha256").read_text(),
                         f"{digest}  {self.archive().name}\n")
        manifest = json.loads((self.output / "source-manifest.json").read_text())
        self.assertEqual(manifest["source_sha256"], digest)
        self.assertEqual(manifest["source_archive"], self.archive().name)
        self.assertEqual(manifest["platform"], "linux-portable")
        self.assertEqual(manifest["files"], {name: hashlib.sha256(content).hexdigest()
                                             for name, content in expected.items()})
        receipt = (self.output / "build-environment.txt").read_text()
        for name in ("dpkg-query", "clang-21", "cmake"):
            self.assertIn("$ " + name, receipt)
            if os.name == "posix":
                self.assertIn(name + " fixture version", receipt)

    def test_source_archive_is_independent_of_paths_and_timestamps(self):
        self.require_success(self.export())
        first = self.archive().read_bytes()
        for path in self.source.rglob("*"):
            os.utime(path, (123456789, 987654321))
        second = self.directory / "another output"
        self.require_success(self.export(second))
        self.assertEqual(self.archive(second).read_bytes(), first)

    def test_bundled_cpu_subset_is_complete_in_source_export(self):
        # BuildKit exercises the actual filter in the clean release pipeline.
        # Keep its required bundled dependency allowlist explicit here, without
        # admitting downloaded vendor trees or implementing a second matcher.
        rules = (ROOT / ".dockerignore").read_text().splitlines()
        self.assertIn("!lib/OpenSubdiv/**", rules)
        self.assertNotIn("!lib/**", rules)
        subset = ROOT / "lib/OpenSubdiv"
        shutil.copytree(subset, self.source / "lib/OpenSubdiv")
        (self.source / "cmake").mkdir()
        cmake = "cmake/HeavenOpenSubdiv.cmake"
        shutil.copyfile(ROOT / cmake, self.source / cmake)
        self.require_success(self.export())
        prefix = "HeavenBench-1.2.3/"
        with tarfile.open(self.archive()) as archive:
            bodies = {member.name.removeprefix(prefix): archive.extractfile(member).read()
                      for member in archive.getmembers() if member.isfile()}
        manifest = json.loads((self.output / "source-manifest.json").read_text())["files"]
        for source in subset.rglob("*"):
            if source.is_file():
                name = source.relative_to(ROOT).as_posix()
                self.assertEqual(bodies[name], source.read_bytes())
                self.assertEqual(manifest[name], hashlib.sha256(source.read_bytes()).hexdigest())
        sources = re.findall(r"\$\{CMAKE_CURRENT_LIST_DIR\}/\.\./(lib/OpenSubdiv/[^\s)]+)",
                             bodies[cmake].decode())
        self.assertTrue(sources, "The exported CPU target must declare its source files")
        self.assertEqual(set(sources), {name for name in bodies
                                       if name.startswith("lib/OpenSubdiv/") and name.endswith(".cpp")})
        for name, body in bodies.items():
            if name.startswith("lib/OpenSubdiv/") and name.endswith((".cpp", ".h")):
                for include in re.findall(r'^\s*#\s*include\s+"([^"]+)"', body.decode(), re.MULTILINE):
                    dependency = posixpath.normpath(posixpath.join(posixpath.dirname(name), include))
                    self.assertIn(dependency, bodies, f"Missing exported header used by {name}")
        for name in ("LICENSE.txt", "NOTICE.txt", "README.heavenbench.md", "opensubdiv/version.h"):
            self.assertIn("lib/OpenSubdiv/" + name, bodies)
        self.assertIn(b"#define OPENSUBDIV_VERSION_NUMBER 30601",
                      bodies["lib/OpenSubdiv/opensubdiv/version.h"])

    @unittest.skipUnless(hasattr(os, "symlink"), "Symlinks are required")
    def test_relative_symlink_is_preserved_without_copying_target(self):
        self.symlink("alias.cpp", "edited.cpp")
        self.require_success(self.export())
        with tarfile.open(self.archive()) as archive:
            member = archive.getmember("HeavenBench-1.2.3/alias.cpp")
            self.assertTrue(member.issym())
            self.assertEqual(member.linkname, "edited.cpp")
        manifest = json.loads((self.output / "source-manifest.json").read_text())
        self.assertEqual(manifest["files"]["alias.cpp"], {"symlink": "edited.cpp"})

    @unittest.skipUnless(hasattr(os, "symlink"), "Symlinks are required")
    def test_escaping_link_is_rejected_before_previous_archive_changes(self):
        self.require_success(self.export())
        original = self.archive().read_bytes()
        outside = self.directory / "private.txt"
        outside.write_bytes(b"outside context")
        self.symlink("escape", outside)
        result = self.export()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.archive().read_bytes(), original)

    @unittest.skipUnless(hasattr(os, "symlink"), "Symlinks are required")
    def test_absolute_link_inside_source_is_rejected_as_nonportable(self):
        self.symlink("absolute.cpp", self.source / "edited.cpp")
        self.assertNotEqual(self.export().returncode, 0)
        self.assertFalse(self.archive().exists())

    def test_output_cannot_contaminate_source_context(self):
        for output in (self.source, self.source / "generated artifacts"):
            with self.subTest(output=output):
                self.assertNotEqual(self.export(output).returncode, 0)
        self.assertFalse((self.source / "generated artifacts").exists())


if __name__ == "__main__":
    unittest.main()
