"""Cross-process transactions and atomic JSON replacement."""

import hashlib
import json
import os
import tempfile
import threading
from contextlib import contextmanager
from pathlib import Path

import portalocker

from .model import Problem, household, validate_assignments


class HouseStore:
    def __init__(self, directory, default_timezone):
        self.directory = Path(directory)
        self.directory.mkdir(parents=True, exist_ok=True)
        self.default_timezone = default_timezone
        self._mutex = threading.RLock()

    @contextmanager
    def transaction(self):
        try:
            with self._mutex, portalocker.Lock(str(self.directory / ".lock"), timeout=10):
                yield self._load()
        except portalocker.exceptions.LockException as error:
            raise Problem("Configuration storage is busy. Retry shortly.", 503, "storage_busy") from error

    def _load(self):
        result = {}
        try:
            for path in sorted(self.directory.glob("*.json")):
                with path.open(encoding="utf-8") as stream:
                    raw = json.load(stream)
                value = household(raw.get("id"), raw, self.default_timezone)
                if value["id"] in result:
                    raise ValueError("Duplicate household ID in storage")
                result[value["id"]] = value
            validate_assignments(result)
        except (OSError, ValueError, TypeError, AttributeError, Problem) as error:
            raise Problem("Stored configuration could not be read. Existing files have been preserved.", 503, "storage_error") from error
        return result

    def save(self, value):
        # IDs never become paths. All callers hold the transaction lock.
        filename = hashlib.sha256(value["id"].encode("utf-8")).hexdigest() + ".json"
        temporary = None
        try:
            with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=self.directory, suffix=".tmp", delete=False) as stream:
                temporary = stream.name
                json.dump(value, stream, indent=2, ensure_ascii=False, allow_nan=False)
                stream.write("\n")
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(temporary, self.directory / filename)
            if os.name != "nt":
                descriptor = os.open(self.directory, os.O_RDONLY | os.O_DIRECTORY)
                try:
                    os.fsync(descriptor)
                finally:
                    os.close(descriptor)
        except OSError as error:
            raise Problem("Configuration could not be saved.", 503, "storage_error") from error
        finally:
            if temporary and os.path.exists(temporary):
                os.unlink(temporary)
