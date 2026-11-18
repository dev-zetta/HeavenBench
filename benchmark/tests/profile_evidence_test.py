#!/usr/bin/env python3
"""CPU filesystem/concurrency oracles for immutable profiling evidence."""
import hashlib
import json
import os
from pathlib import Path
import select
import signal
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
from profile_harness import EvidenceError, EvidenceStore
from profile_harness import evidence

POSIX_STORE = os.name == "posix" and hasattr(os, "O_NOFOLLOW") and hasattr(os, "O_DIRECTORY")


@unittest.skipUnless(POSIX_STORE, "Requires POSIX no-follow directory descriptors")
class EvidenceTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name)
        self.root = self.base / "store"
        self.store = EvidenceStore(self.root, create=True)
        self.addCleanup(self.store.close)

    def source(self, data=b"source bytes"):
        path = self.base / "source.bin"
        path.write_bytes(data)
        return path

    def object_path(self, ref):
        return self.root / "objects" / ref["sha256"]

    def raw_record(self, data):
        ref = dict(sha256=hashlib.sha256(data).hexdigest(), bytes=len(data), format="record")
        self.object_path(ref).write_bytes(data)
        return ref

    def assert_no_temporary_files(self):
        self.assertEqual(list(self.root.rglob(".tmp-*")), [])

    def test_canonical_record_and_reopen(self):
        first = self.store.put_record("settings", {"z": 2, "a": "é"})
        second = self.store.put_record("settings", {"a": "é", "z": 2})
        self.assertEqual(first, second)
        self.store.publish("cohort-one/A1/settings.json", first)
        with EvidenceStore(self.root) as reopened:
            ref = reopened.load("cohort-one/A1/settings.json")
            self.assertEqual(ref, first)
            self.assertEqual(reopened.verify(ref), dict(schema=1, kind="settings", payload={"a": "é", "z": 2}, refs=[]))
        self.assert_no_temporary_files()

    def test_exists_distinguishes_absence_from_corruption_and_links(self):
        self.assertFalse(self.store.exists("missing.json"))
        self.assertFalse(self.store.exists("new-cohort/A1/missing.json"))
        self.assertEqual(list((self.root / "entries").iterdir()), [])
        ref = self.store.put_record("exists", {})
        self.store.publish("cohort/A1/present.json", ref)
        self.assertTrue(self.store.exists("cohort/A1/present.json"))
        self.object_path(ref).unlink()
        self.assertTrue(self.store.exists("cohort/A1/present.json"))
        broken = self.root / "entries/broken.json"
        broken.write_bytes(b"{")
        with self.assertRaises(EvidenceError):
            self.store.exists("broken.json")
        link = self.root / "entries/link.json"
        link.symlink_to(self.base / "missing-target")
        with self.assertRaises(EvidenceError):
            self.store.exists("link.json")
        (self.root / "entries/ancestor-link").symlink_to(self.base, target_is_directory=True)
        with self.assertRaises(EvidenceError):
            self.store.exists("ancestor-link/missing.json")
        (self.root / "entries/not-a-directory").write_bytes(b"file")
        with self.assertRaises(EvidenceError):
            self.store.exists("not-a-directory/missing.json")

    def test_blob_is_copied_and_independent_of_live_source(self):
        source = self.source(b"snapshot")
        ref = self.store.put_file(source)
        source.write_bytes(b"later live contents")
        self.assertEqual(self.object_path(ref).read_bytes(), b"snapshot")
        self.assertIsNone(self.store.verify(ref))
        self.assertEqual(ref["format"], "blob")

    def test_streamed_file_matches_independent_digest(self):
        data = bytes(range(256)) * 12000
        ref = self.store.put_file(self.source(data))
        self.assertEqual(ref, dict(sha256=hashlib.sha256(data).hexdigest(), bytes=len(data), format="blob"))
        self.store.verify(ref)

    def test_cohort_names_are_independent_and_collision_keeps_original(self):
        one = self.store.put_record("boundary", {"cohort": 1})
        two = self.store.put_record("boundary", {"cohort": 2})
        self.store.publish("cohort-1/A1/boundary.json", one)
        self.store.publish("cohort-2/A1/boundary.json", two)
        path = self.root / "entries/cohort-1/A1/boundary.json"
        original = path.read_bytes()
        for ref in (one, two):
            with self.assertRaises(FileExistsError):
                self.store.publish("cohort-1/A1/boundary.json", ref)
            self.assertEqual(path.read_bytes(), original)
        self.assertEqual(self.store.load("cohort-2/A1/boundary.json"), two)
        self.assert_no_temporary_files()

    def test_nested_grandchild_tamper_is_not_hidden_by_parent_passed(self):
        blob = self.store.put_file(self.source(b"v1"))
        leaf = self.store.put_record("boundary", {"passed": True}, [blob])
        root = self.store.put_record("cohort", {"passed": True}, [leaf])
        self.store.publish("cohort/A1/result.json", root)
        self.store.verify(self.store.load("cohort/A1/result.json"))
        self.object_path(blob).chmod(0o600)
        self.object_path(blob).write_bytes(b"v2")
        with self.assertRaisesRegex(EvidenceError, "digest|content"):
            self.store.verify(self.store.load("cohort/A1/result.json"))

    def test_nested_deletion_and_missing_ref_reject(self):
        child = self.store.put_record("leaf", {})
        parent = self.store.put_record("parent", {"passed": True}, [child])
        self.object_path(child).unlink()
        with self.assertRaises(EvidenceError):
            self.store.verify(parent)
        with self.assertRaises(EvidenceError):
            self.store.put_record("new parent", {}, [child])

    def test_conflicting_ref_types_reject(self):
        child = self.store.put_record("child", {})
        incompatible = {**child, "format": "blob"}
        with self.assertRaisesRegex(EvidenceError, "Conflicting"):
            self.store.put_record("parent", {}, [child, incompatible])
        incompatible = {**child, "bytes": child["bytes"] + 1}
        with self.assertRaisesRegex(EvidenceError, "Conflicting"):
            self.store.put_record("parent", {}, [child, incompatible])

    def test_dag_reads_shared_objects_once_per_verification(self):
        blob = self.store.put_file(self.source())
        a = self.store.put_record("a", {}, [blob])
        b = self.store.put_record("b", {}, [blob])
        parent = self.store.put_record("parent", {}, [a, b])
        with mock.patch.object(self.store, "_read_verified", wraps=self.store._read_verified) as reader:
            self.store.verify(parent)
        self.assertEqual(reader.call_count, 4)

    def test_cycle_graph_fails_before_recursion_exhaustion(self):
        # A genuine SHA256 fixed-point cycle cannot be constructed. Exercise
        # graph traversal independently of the separately tested hash reader.
        a = dict(sha256="a" * 64, bytes=256, format="record")
        b = dict(sha256="b" * 64, bytes=256, format="record")
        graph = {a["sha256"]: evidence._canonical(dict(schema=1, kind="a", payload={}, refs=[b])),
                 b["sha256"]: evidence._canonical(dict(schema=1, kind="b", payload={}, refs=[a]))}
        with mock.patch.object(self.store, "_read_verified", side_effect=lambda r: graph[r["sha256"]]):
            with self.assertRaisesRegex(EvidenceError, "Cyclic"):
                self.store.verify(a)

    def test_graph_depth_is_bounded(self):
        leaf = self.store.put_record("leaf", {})
        child = self.store.put_record("child", {}, [leaf])
        parent = self.store.put_record("parent", {}, [child])
        with mock.patch.object(evidence, "_MAX_VERIFY_DEPTH", 1):
            with self.assertRaisesRegex(EvidenceError, "depth"):
                self.store.verify(parent)

    def test_invalid_ref_shapes_and_nonfinite_payload_reject(self):
        valid = self.store.put_record("valid", {})
        variants = [{**valid, "extra": True}, {**valid, "bytes": True}, {**valid, "bytes": -1},
                    {**valid, "sha256": "A" * 64}, {**valid, "format": "log"}]
        for ref in variants:
            with self.subTest(ref=ref), self.assertRaises(EvidenceError):
                self.store.verify(ref)
        for value in (float("nan"), float("inf"), {"not": b"JSON"}):
            with self.subTest(value=value), self.assertRaises(EvidenceError):
                self.store.put_record("invalid", value)

    def test_incomplete_noncanonical_and_wrong_type_objects_reject(self):
        for data in (b"{", b'{"schema":true,"kind":"x","payload":{},"refs":[]}\n',
                     b'{"schema":1,"kind":"x","refs":[]}\n',
                     b'{"schema":1,"kind":"x","payload":NaN,"refs":[]}\n',
                     b'{"schema":1,"kind":"x","payload":{},"refs":[]}\n'):
            with self.subTest(data=data):
                ref = self.raw_record(data)
                with self.assertRaises(EvidenceError):
                    self.store.verify(ref)
        name = self.root / "entries/incomplete.json"
        name.write_bytes(b'{"sha256":')
        with self.assertRaises(EvidenceError):
            self.store.load("incomplete.json")

    def test_paths_and_symlink_escapes_reject(self):
        ref = self.store.put_record("ok", {})
        for name in ("/absolute", "../outside", "a/../outside", "a//b", "./a", "a\\b", ".tmp-forged"):
            with self.subTest(name=name), self.assertRaises(EvidenceError):
                self.store.publish(name, ref)
        outside = self.base / "outside"
        outside.mkdir()
        (self.root / "entries/escape").symlink_to(outside, target_is_directory=True)
        with self.assertRaises(EvidenceError):
            self.store.publish("escape/result.json", ref)
        self.assertEqual(list(outside.iterdir()), [])
        source = self.source()
        link = self.base / "source-link"
        link.symlink_to(source)
        with self.assertRaises(EvidenceError):
            self.store.put_file(link)
        object_path = self.object_path(ref)
        object_path.unlink()
        object_path.symlink_to(source)
        with self.assertRaises(EvidenceError):
            self.store.verify(ref)

    def test_root_and_object_directory_symlinks_reject(self):
        link = self.base / "root-link"
        link.symlink_to(self.root, target_is_directory=True)
        with self.assertRaises(EvidenceError):
            EvidenceStore(link)
        (self.root / "objects").rename(self.root / "saved-objects")
        (self.root / "objects").symlink_to(self.root / "saved-objects", target_is_directory=True)
        with self.assertRaises(EvidenceError):
            self.store.put_record("unsafe", {})

    def test_source_mutation_during_stream_does_not_publish_object(self):
        source = self.source(b"x" * (evidence._CHUNK_BYTES + 1))
        original_chunks = evidence._chunks
        def changing(fd):
            for index, block in enumerate(original_chunks(fd)):
                if index == 0:
                    source.write_bytes(b"changed")
                yield block
        with mock.patch.object(evidence, "_chunks", side_effect=changing):
            with self.assertRaisesRegex(EvidenceError, "Source changed"):
                self.store.put_file(source)
        self.assertEqual(list((self.root / "objects").iterdir()), [])

    def test_source_path_replacement_during_copy_is_rejected(self):
        source = self.source(b"original")
        original_chunks = evidence._chunks
        def replacing(fd):
            for block in original_chunks(fd):
                replacement = self.base / "replacement"
                replacement.write_bytes(b"original")
                replacement.replace(source)
                yield block
        with mock.patch.object(evidence, "_chunks", side_effect=replacing):
            with self.assertRaisesRegex(EvidenceError, "Source changed"):
                self.store.put_file(source)
        self.assertEqual(list((self.root / "objects").iterdir()), [])

    def test_size_limits_and_failed_publication_leave_no_partial_entry(self):
        with EvidenceStore(self.base / "small", create=True, max_object_bytes=8) as small:
            with self.assertRaises(EvidenceError):
                small.put_file(self.source(b"too many bytes"))
            ref = small.put_file(self.source(b"small"))
            small.publish("cohort/A1/blob", ref)
            self.assertEqual(small.load("cohort/A1/blob"), ref)
        ref = self.store.put_record("ok", {})
        with mock.patch.object(evidence.os, "link", side_effect=OSError("simulated publication failure")):
            with self.assertRaises(OSError):
                self.store.publish("cohort/A1/failed.json", ref)
        self.assertFalse((self.root / "entries/cohort/A1/failed.json").exists())
        self.assert_no_temporary_files()

    def test_failed_stream_write_leaves_no_complete_object(self):
        original_write = evidence._write_all
        def fail(fd, data):
            original_write(fd, data[:3])
            raise OSError("simulated interrupted write")
        with mock.patch.object(evidence, "_write_all", side_effect=fail):
            with self.assertRaises(OSError):
                self.store.put_record("not complete", {})
        self.assertEqual(list((self.root / "objects").iterdir()), [])

    def test_configurable_object_and_recursive_closure_limits(self):
        with EvidenceStore(self.base / "large-policy", create=True,
                           max_object_bytes=512 * 1024 * 1024,
                           max_verify_bytes=2 * 1024 * 1024 * 1024) as store:
            blob = store.put_file(self.source(b"bounded"))
            root = store.put_record("root", {}, [blob])
            total = root["bytes"] + blob["bytes"]
            with EvidenceStore(store.root, max_verify_bytes=total - 1) as limited:
                with self.assertRaisesRegex(EvidenceError, "closure"):
                    limited.verify(root)
            with EvidenceStore(store.root, max_verify_bytes=total) as exact:
                self.assertEqual(exact.verify(root)["kind"], "root")
        for keyword in ("max_object_bytes", "max_verify_bytes"):
            for invalid in (0, -1, True, 1.5):
                with self.subTest(keyword=keyword, invalid=invalid), self.assertRaises(EvidenceError):
                    EvidenceStore(self.root, **{keyword: invalid})

    def test_copy_blob_is_verified_atomic_and_a_separate_inode(self):
        ref = self.store.put_file(self.source(b"frozen binary"))
        destination = self.base / "private-binary"
        self.assertEqual(self.store.copy_blob(ref, destination), ref)
        self.assertEqual(destination.read_bytes(), b"frozen binary")
        self.assertNotEqual(destination.stat().st_ino, self.object_path(ref).stat().st_ino)
        destination.chmod(0o600)
        destination.write_bytes(b"private edit")
        self.store.verify(ref)
        with self.assertRaises(FileExistsError):
            self.store.copy_blob(ref, destination)
        self.assertEqual(destination.read_bytes(), b"private edit")
        self.assert_no_temporary_files()

    def test_copy_blob_rejects_corruption_without_publishing_destination(self):
        ref = self.store.put_file(self.source(b"good"))
        self.object_path(ref).chmod(0o600)
        self.object_path(ref).write_bytes(b"bad!")
        destination = self.base / "not-complete"
        with self.assertRaisesRegex(EvidenceError, "digest"):
            self.store.copy_blob(ref, destination)
        self.assertFalse(destination.exists())
        self.assertEqual(list(self.base.glob(".tmp-*")), [])

    def test_copy_blob_rejects_records_and_unsafe_destinations(self):
        record = self.store.put_record("record", {})
        with self.assertRaises(EvidenceError):
            self.store.copy_blob(record, self.base / "record-copy")
        ref = self.store.put_file(self.source(b"unchanged"))
        target = self.base / "outside"
        target.write_bytes(b"original")
        link = self.base / "destination-link"
        link.symlink_to(target)
        with self.assertRaises(FileExistsError):
            self.store.copy_blob(ref, link)
        self.assertEqual(target.read_bytes(), b"original")
        parent_link = self.base / "parent-link"
        parent_link.symlink_to(self.base, target_is_directory=True)
        with self.assertRaises(EvidenceError):
            self.store.copy_blob(ref, parent_link / "escaped")
        self.assertFalse((self.base / "escaped").exists())

    def test_special_files_are_rejected_without_blocking(self):
        fifo = self.base / "source-fifo"
        os.mkfifo(fifo)
        with self.assertRaises(EvidenceError):
            self.store.put_file(fifo)
        ref = self.store.put_record("record", {})
        self.object_path(ref).unlink()
        os.mkfifo(self.object_path(ref))
        with self.assertRaises(EvidenceError):
            self.store.verify(ref)
        named_fifo = self.root / "entries/fifo"
        os.mkfifo(named_fifo)
        with self.assertRaises(EvidenceError):
            self.store.load("fifo")

    def concurrent(self, names, values):
        children = []
        try:
            for name, value in zip(names, values):
                child = subprocess.Popen([sys.executable, "-E", "-B", str(Path(__file__).resolve()),
                                          "--writer", str(self.root), name, value],
                                         stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                         stderr=subprocess.PIPE, text=True, start_new_session=True)
                children.append(child)
            for child in children:
                self.assertTrue(select.select([child.stdout], [], [], 5)[0], "CPU writer readiness timeout")
                self.assertEqual(child.stdout.readline().strip(), "READY")
            for child in children:
                child.stdin.write("GO\n")
                child.stdin.flush()
            results = []
            for child in children:
                stdout, stderr = child.communicate(timeout=5)
                self.assertEqual(stderr, "")
                results.append((child.returncode, json.loads(stdout)))
            return results
        finally:
            for child in children:
                if child.poll() is None:
                    os.killpg(child.pid, signal.SIGKILL)
                    child.wait(timeout=2)

    def test_real_concurrent_same_name_only_one_wins_without_overwrite(self):
        results = self.concurrent(["cohort/A1/result", "cohort/A1/result"], ["first", "second"])
        self.assertEqual(sorted(code for code, _ in results), [0, 17])
        winner = next(value for code, value in results if code == 0)
        actual = self.store.load("cohort/A1/result")
        self.assertEqual(actual, winner["ref"])
        self.assertEqual(self.store.verify(actual)["payload"], {"value": winner["value"]})
        self.assert_no_temporary_files()

    def test_real_concurrent_equal_objects_deduplicate_safely(self):
        results = self.concurrent(["cohort/A1/result", "cohort/B1/result"], ["same", "same"])
        self.assertEqual([code for code, _ in results], [0, 0])
        self.assertEqual(results[0][1]["ref"], results[1][1]["ref"])
        self.assertEqual(len(list((self.root / "objects").iterdir())), 1)
        self.assert_no_temporary_files()


def writer():
    root, name, value = sys.argv[2:]
    with EvidenceStore(root) as store:
        print("READY", flush=True)
        if sys.stdin.readline().strip() != "GO":
            return 2
        ref = store.put_record("concurrent", {"value": value})
        try:
            store.publish(name, ref)
            code = 0
        except FileExistsError:
            code = 17
        print(json.dumps(dict(ref=ref, value=value)), flush=True)
        return code


if __name__ == "__main__":
    if sys.argv[1:2] == ["--writer"]:
        raise SystemExit(writer())
    unittest.main()
