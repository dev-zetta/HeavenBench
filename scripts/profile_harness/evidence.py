"""Small immutable profiling store, using POSIX no-follow and no-replace operations.

Objects are content-addressed raw blobs or canonical JSON records. Named entries
contain only typed references; ``verify(load(name))`` verifies the whole closure
before returning a root record body. Payloads are opaque: only explicit ``refs``
are graph edges. Mutable progress is deliberately outside this store.
"""
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import stat

DEFAULT_MAX_OBJECT_BYTES = 128 * 1024 * 1024
DEFAULT_MAX_VERIFY_BYTES = 512 * 1024 * 1024
_MAX_VERIFY_OBJECTS = 100_000
_MAX_VERIFY_DEPTH = 128
_CHUNK_BYTES = 1024 * 1024


class EvidenceError(ValueError):
    """Malformed, changed, missing, unsafe or incomplete profiling evidence."""


def _require(value, reason):
    if not value:
        raise EvidenceError(reason)


def _canonical(value):
    try:
        return (json.dumps(value, sort_keys=True, separators=(",", ":"),
                           ensure_ascii=False, allow_nan=False) + "\n").encode("utf-8")
    except (TypeError, ValueError, UnicodeError) as error:
        raise EvidenceError("Evidence is not finite JSON") from error


def _reference(value, limit):
    _require(isinstance(value, dict) and set(value) == {"sha256", "bytes", "format"},
             "A reference needs exactly sha256, bytes and format")
    _require(isinstance(value["sha256"], str) and re.fullmatch(r"[0-9a-f]{64}", value["sha256"]),
             "Invalid SHA-256 reference")
    _require(type(value["bytes"]) is int and 0 <= value["bytes"] <= limit,
             "Invalid or oversized reference length")
    _require(value["format"] in ("record", "blob"), "Unknown evidence format")
    return {key: value[key] for key in ("sha256", "bytes", "format")}


def _decode(data):
    def invalid_constant(_value):
        raise EvidenceError("Nonfinite JSON evidence")
    try:
        return json.loads(data.decode("utf-8"), parse_constant=invalid_constant)
    except (ValueError, UnicodeError) as error:
        raise EvidenceError("Incomplete or invalid JSON evidence") from error


def _snapshot(info, include_ctime=False):
    values = (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns)
    return values + (info.st_ctime_ns,) if include_ctime else values


def _chunks(fd):
    while block := os.read(fd, _CHUNK_BYTES):
        yield block


def _write_all(fd, data):
    view = memoryview(data)
    while view:
        written = os.write(fd, view)
        _require(written > 0, "Incomplete evidence write")
        view = view[written:]


def _child_directory(fd, name, create):
    flags = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW
    try:
        return os.open(name, flags, dir_fd=fd)
    except FileNotFoundError:
        if not create:
            raise
    try:
        os.mkdir(name, 0o700, dir_fd=fd)
        os.fsync(fd)
    except FileExistsError:
        pass
    return os.open(name, flags, dir_fd=fd)


def _absolute_directory(path, create=False):
    """Walk from / with directory descriptors; never follow an ancestor link."""
    _require(os.name == "posix" and hasattr(os, "O_NOFOLLOW") and hasattr(os, "O_DIRECTORY"),
             "Profiling evidence requires POSIX no-follow directory operations")
    fd = os.open("/", os.O_RDONLY | os.O_DIRECTORY)
    try:
        for part in Path(path).parts[1:]:
            child = _child_directory(fd, part, create)
            os.close(fd)
            fd = child
        return fd
    except OSError as error:
        os.close(fd)
        raise EvidenceError("Missing or unsafe evidence directory") from error


class EvidenceStore:
    """Immutable objects and cohort/window-qualified named references.

    ``create=True`` creates the root/objects/entries directories idempotently;
    it never replaces existing files. Named publication always rejects an
    existing name, including an identical reference. Object deduplication is
    allowed only after verifying the existing bytes. ``load`` returns a typed
    reference; ``verify`` returns the root record body, or None for a blob.
    """

    def __init__(self, root, create=False, max_object_bytes=DEFAULT_MAX_OBJECT_BYTES,
                 max_verify_bytes=DEFAULT_MAX_VERIFY_BYTES):
        _require(type(max_object_bytes) is int and max_object_bytes > 0, "Invalid object limit")
        _require(type(max_verify_bytes) is int and max_verify_bytes > 0, "Invalid closure limit")
        self.root = Path(os.path.abspath(root))
        self.max_object_bytes = max_object_bytes
        self.max_verify_bytes = max_verify_bytes
        self._fd = _absolute_directory(self.root, create)
        try:
            for name in ("objects", "entries"):
                with self._directory((name,), create=create):
                    pass
        except BaseException:
            self.close()
            raise

    def close(self):
        fd = getattr(self, "_fd", None)
        if fd is not None:
            os.close(fd)
            self._fd = None

    def __enter__(self):
        return self

    def __exit__(self, *_args):
        self.close()

    def __del__(self):
        self.close()

    @contextmanager
    def _directory(self, parts, create=False):
        _require(self._fd is not None, "Evidence store is closed")
        # The pathname must still denote the originally opened root.
        current = _absolute_directory(self.root)
        try:
            _require(_snapshot(os.fstat(current))[:2] == _snapshot(os.fstat(self._fd))[:2],
                     "Evidence root was replaced")
        finally:
            os.close(current)
        fd = os.dup(self._fd)
        try:
            for part in parts:
                child = _child_directory(fd, part, create)
                os.close(fd)
                fd = child
        except OSError as error:
            os.close(fd)
            raise EvidenceError("Missing or unsafe evidence path") from error
        try:
            yield fd
        finally:
            os.close(fd)

    def _install(self, directory, name, blocks, *, deduplicate=False, format=None, max_bytes=None):
        temporary = ".tmp-" + secrets.token_hex(16)
        fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW,
                     0o600, dir_fd=directory)
        digest, length = hashlib.sha256(), 0
        try:
            for block in blocks:
                length += len(block)
                limit = max_bytes if max_bytes is not None else (self.max_object_bytes if deduplicate else 4096)
                _require(length <= limit, "Object exceeds the evidence limit")
                digest.update(block)
                _write_all(fd, block)
            os.fchmod(fd, 0o444)
            os.fsync(fd)
            os.close(fd)
            fd = None
            reference = {"sha256": digest.hexdigest(), "bytes": length, "format": format}
            destination = digest.hexdigest() if deduplicate else name
            try:
                os.link(temporary, destination, src_dir_fd=directory, dst_dir_fd=directory,
                        follow_symlinks=False)
            except FileExistsError:
                if not deduplicate:
                    raise
                self._read_verified(reference)
            os.unlink(temporary, dir_fd=directory)
            os.fsync(directory)
            return reference
        finally:
            if fd is not None:
                os.close(fd)
            try:
                os.unlink(temporary, dir_fd=directory)
            except FileNotFoundError:
                pass

    def put_file(self, path):
        """Stream a regular no-follow source; reject replacement/mutation while copying."""
        source = Path(os.path.abspath(path))
        parent = _absolute_directory(source.parent)
        fd = None
        try:
            fd = os.open(source.name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=parent)
            before = os.fstat(fd)
            _require(stat.S_ISREG(before.st_mode) and before.st_size <= self.max_object_bytes,
                     "Source must be a bounded regular file")

            def stable_chunks():
                yield from _chunks(fd)
                after = os.fstat(fd)
                named = os.stat(source.name, dir_fd=parent, follow_symlinks=False)
                _require(stat.S_ISREG(named.st_mode) and
                         _snapshot(before, True) == _snapshot(after, True) == _snapshot(named, True),
                         "Source changed while being snapshotted")

            with self._directory(("objects",)) as objects:
                return self._install(objects, None, stable_chunks(), deduplicate=True, format="blob")
        except OSError as error:
            raise EvidenceError("Could not snapshot a safe source file") from error
        finally:
            if fd is not None:
                os.close(fd)
            os.close(parent)

    def copy_blob(self, ref, destination):
        """Atomically materialize a verified blob as a separate readonly inode.

        The destination parent must exist and contain no symlink ancestors.
        Existing destinations, including links, are never replaced. A caller
        may make its new private binary executable after this method returns.
        """
        reference = _reference(ref, self.max_object_bytes)
        _require(reference["format"] == "blob", "Only blobs can be materialized")
        destination = Path(os.path.abspath(destination))
        parent = _absolute_directory(destination.parent)
        source_fd = None
        try:
            try:
                os.stat(destination.name, dir_fd=parent, follow_symlinks=False)
            except FileNotFoundError:
                pass
            else:
                raise FileExistsError(str(destination))
            with self._directory(("objects",)) as objects:
                source_fd = os.open(reference["sha256"],
                                    os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=objects)
            before = os.fstat(source_fd)
            _require(stat.S_ISREG(before.st_mode) and before.st_size == reference["bytes"],
                     "Blob type/length differs")

            def checked_chunks():
                digest, length = hashlib.sha256(), 0
                for block in _chunks(source_fd):
                    length += len(block)
                    _require(length <= reference["bytes"], "Blob grew during materialization")
                    digest.update(block)
                    yield block
                _require(length == reference["bytes"] and digest.hexdigest() == reference["sha256"]
                         and _snapshot(before) == _snapshot(os.fstat(source_fd)),
                         "Blob changed or digest differs during materialization")

            observed = self._install(parent, destination.name, checked_chunks(), format="blob",
                                     max_bytes=self.max_object_bytes)
            _require(observed == reference, "Materialized blob identity differs")
            return reference
        finally:
            if source_fd is not None:
                os.close(source_fd)
            os.close(parent)

    def put_record(self, kind, payload, refs=()):
        _require(isinstance(kind, str) and bool(kind) and "\0" not in kind, "Invalid record kind")
        references = [_reference(ref, self.max_object_bytes) for ref in refs]
        self._verify_many(references)
        data = _canonical({"schema": 1, "kind": kind, "payload": payload, "refs": references})
        with self._directory(("objects",)) as objects:
            return self._install(objects, None, (data,), deduplicate=True, format="record")

    @staticmethod
    def _name(relative_name):
        name = os.fspath(relative_name)
        _require(isinstance(name, str) and name and not name.startswith("/") and
                 "\\" not in name and "\0" not in name, "Unsafe evidence entry name")
        parts = name.split("/")
        _require(all(part and not part.startswith(".") for part in parts),
                 "Evidence entry names must be explicit relative components")
        return parts

    def publish(self, relative_name, ref):
        """Seal a named entry once, with no-replace atomic publication."""
        reference = _reference(ref, self.max_object_bytes)
        self.verify(reference)
        parts = self._name(relative_name)
        with self._directory(("entries", *parts[:-1]), create=True) as directory:
            self._install(directory, parts[-1], (_canonical(reference),))
        return reference

    def load(self, relative_name):
        parts = self._name(relative_name)
        with self._directory(("entries", *parts[:-1])) as directory:
            fd = None
            try:
                fd = os.open(parts[-1], os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=directory)
                before = os.fstat(fd)
                _require(stat.S_ISREG(before.st_mode), "Entry is not a regular file")
                data = os.read(fd, 4097)
                _require(len(data) <= 4096 and len(data) == before.st_size and
                         _snapshot(before) == _snapshot(os.fstat(fd)),
                         "Oversized, incomplete or changing named entry")
            except OSError as error:
                raise EvidenceError("Missing or unsafe named entry") from error
            finally:
                if fd is not None:
                    os.close(fd)
        reference = _reference(_decode(data), self.max_object_bytes)
        _require(data == _canonical(reference), "Named entry is incomplete or noncanonical")
        return reference

    def exists(self, relative_name):
        """Check a named entry without conflating absence with unsafe/corrupt paths.

        This validates the named reference, not its object closure. An entry
        whose object was deleted still exists and must never be overwritten.
        """
        try:
            self.load(relative_name)
            return True
        except EvidenceError as error:
            if isinstance(error.__cause__, FileNotFoundError):
                return False
            raise

    def _read_verified(self, ref):
        with self._directory(("objects",)) as objects:
            fd = None
            try:
                fd = os.open(ref["sha256"], os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=objects)
                before = os.fstat(fd)
                _require(stat.S_ISREG(before.st_mode) and before.st_size == ref["bytes"],
                         "Object type/length differs")
                digest, length, data = hashlib.sha256(), 0, bytearray()
                for block in _chunks(fd):
                    length += len(block)
                    _require(length <= ref["bytes"], "Object grew during verification")
                    digest.update(block)
                    if ref["format"] == "record":
                        data.extend(block)
                _require(_snapshot(before) == _snapshot(os.fstat(fd)) and length == ref["bytes"]
                         and digest.hexdigest() == ref["sha256"], "Object content changed or digest differs")
                return bytes(data) if ref["format"] == "record" else None
            except OSError as error:
                raise EvidenceError("Missing or unsafe evidence object") from error
            finally:
                if fd is not None:
                    os.close(fd)

    def _verify_many(self, refs):
        seen, active, total = {}, set(), 0

        def visit(value, depth):
            nonlocal total
            ref = _reference(value, self.max_object_bytes)
            key = ref["sha256"]
            _require(depth <= _MAX_VERIFY_DEPTH, "Evidence graph exceeds depth limit")
            _require(key not in seen or seen[key] == ref, "Conflicting typed references")
            _require(key not in active, "Cyclic evidence references")
            if key in seen:
                return None
            seen[key] = ref
            total += ref["bytes"]
            _require(len(seen) <= _MAX_VERIFY_OBJECTS and total <= self.max_verify_bytes,
                     "Evidence closure exceeds verification limit")
            active.add(key)
            data = self._read_verified(ref)
            body = None
            if ref["format"] == "record":
                body = _decode(data)
                _require(isinstance(body, dict) and set(body) == {"schema", "kind", "payload", "refs"}
                         and type(body["schema"]) is int and body["schema"] == 1
                         and isinstance(body["kind"], str) and bool(body["kind"])
                         and "\0" not in body["kind"] and isinstance(body["refs"], list),
                         "Invalid typed record envelope")
                _require(data == _canonical(body), "Noncanonical record object")
                for child in body["refs"]:
                    visit(child, depth + 1)
            active.remove(key)
            return body

        return [visit(ref, 0) for ref in refs]

    def verify(self, ref):
        """Rehash and recursively verify every explicit edge; never trust a passed flag."""
        return self._verify_many((ref,))[0]
