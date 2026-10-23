"""Bounded local controller transport, validation, and persistent operation reservations."""

import hashlib
import http.client
import json
import math
import os
import re
import socket
import tempfile
import threading
from contextlib import contextmanager
from ipaddress import IPv4Address, IPv4Network
from pathlib import Path

import portalocker

from .model import Problem

SETUP_IP = "192.168.0.230"
CONFIG_FIELDS = {"ssid", "password", "ip", "subnet", "gateway", "dns", "api_host", "api_port", "sensor_mac"}
MAC = re.compile(r"(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}")
DEVICE_ID = re.compile(r"[0-9a-fA-F]{32}")


class ControllerFailure(Problem):
    def __init__(self, message, status=502, code="controller_unavailable", rejected=False):
        super().__init__(message, status, code)
        self.rejected = rejected


class ControllerTransport:
    """Direct IPv4 HTTP only: no environment proxies, DNS targets, or redirects."""

    def request(self, address, method, path, payload=None):
        address = str(IPv4Address(address))
        connection = http.client.HTTPConnection(address, 80, timeout=15)
        try:
            encoded = json.dumps(payload, allow_nan=False).encode() if payload is not None else None
            connection.request(method, path, encoded, {"Content-Type": "application/json", "Connection": "close"})
            response = connection.getresponse()
            raw = response.read(8193)
            if response.status in (400, 409):
                raise ControllerFailure("The controller rejected the command; its settings were not changed.", response.status, "controller_rejected", True)
            if response.status not in (200, 202) or len(raw) > 8192:
                raise ControllerFailure("The controller returned an unsupported response.", 502, "controller_response_invalid")
            try:
                value = json.loads(raw)
            except (ValueError, UnicodeError):
                raise ControllerFailure("The controller returned invalid JSON.", 502, "controller_response_invalid") from None
            if not isinstance(value, dict):
                raise ControllerFailure("The controller returned an invalid response.", 502, "controller_response_invalid")
            return value
        except (socket.timeout, TimeoutError):
            raise ControllerFailure("The controller did not respond in time. A sent command may already have been applied.", 504, "controller_timeout") from None
        except (OSError, http.client.HTTPException):
            raise ControllerFailure("The controller could not be reached. Check its power and Wi-Fi connection.") from None
        finally:
            connection.close()


def address(value, label):
    try:
        if not isinstance(value, str):
            raise ValueError()
        parsed = IPv4Address(value)
        if not 0 < parsed.packed[0] < 224 or parsed.packed[0] == 127:
            raise ValueError()
        return str(parsed)
    except ValueError:
        raise Problem(f"{label} must be a valid IPv4 host address.") from None


def config_patch(raw, current, allow_empty_sensor=False):
    if not isinstance(raw, dict) or not raw or set(raw) - CONFIG_FIELDS or any(v is None for v in raw.values()):
        raise Problem("Supply a nonempty controller settings update with supported, non-null fields.")
    candidate = {key: current.get(key) for key in CONFIG_FIELDS - {"password"}}
    candidate.update(raw)
    def byte_length(value):
        try:
            return len(value.encode("utf-8")) if isinstance(value, str) else -1
        except UnicodeError:
            return -1

    ssid = candidate.get("ssid")
    if not isinstance(ssid, str) or not 1 <= byte_length(ssid) <= 32 or any(ord(c) < 32 or ord(c) == 127 for c in ssid):
        raise Problem("Wi-Fi SSID must contain 1 to 32 bytes without control characters.")
    if "password" in raw:
        password = raw["password"]
        if not isinstance(password, str) or (password and not 8 <= len(password) <= 63) or any(not 32 <= ord(c) <= 126 for c in password):
            raise Problem("Wi-Fi password must be empty for an open network, or contain 8 to 63 printable ASCII characters.")
    for name in ("ip", "gateway", "dns"):
        candidate[name] = address(candidate.get(name), name.upper())
    try:
        subnet = candidate["subnet"]
        if not isinstance(subnet, str) or "." not in subnet:
            raise ValueError()
        network = IPv4Network(f"{candidate['ip']}/{subnet}", strict=False)
        if not 1 <= network.prefixlen <= 30 or str(network.netmask) != subnet:
            raise ValueError()
        if IPv4Address(candidate["ip"]) in (network.network_address, network.broadcast_address):
            raise ValueError()
        if IPv4Address(candidate["gateway"]) not in network or IPv4Address(candidate["gateway"]) in (network.network_address, network.broadcast_address, IPv4Address(candidate["ip"])):
            raise ValueError()
    except (ValueError, KeyError):
        raise Problem("Use a contiguous subnet mask, a usable controller IP, and a gateway in the same subnet.") from None
    host = candidate.get("api_host")
    if not isinstance(host, str) or not re.fullmatch(r"[A-Za-z0-9.-]{1,127}", host):
        raise Problem("API host must be an IP address or hostname of at most 127 characters.")
    port = candidate.get("api_port")
    if isinstance(port, bool) or not isinstance(port, int) or not 1 <= port <= 65535:
        raise Problem("API port must be an integer from 1 to 65535.")
    mac = candidate.get("sensor_mac")
    if not isinstance(mac, str) or not (MAC.fullmatch(mac) or (allow_empty_sensor and mac == "")):
        raise Problem("Sensor MAC must contain six hexadecimal octets separated by colons.")
    candidate["sensor_mac"] = mac.upper()
    return {key: candidate[key] for key in raw}


def clean_status(raw):
    """Whitelist fields so firmware/debug additions cannot expose passwords."""
    try:
        if not isinstance(raw, dict) or not isinstance(raw.get("device_id"), str) or not DEVICE_ID.fullmatch(raw["device_id"]):
            raise ValueError()
        result = {"device_id": raw["device_id"].lower()}
        for name in ("wifi_connected", "ble_scanning", "heater_on", "setup_mode", "restart_pending"):
            if not isinstance(raw.get(name), bool):
                raise ValueError()
            result[name] = raw[name]
        for name in ("uptime_ms", "wifi_status"):
            if isinstance(raw.get(name), bool) or not isinstance(raw.get(name), int) or not 0 <= raw[name] <= 0xFFFFFFFF:
                raise ValueError()
            result[name] = raw[name]
        for name in ("api_age_ms", "sensor_age_ms", "ble_age_ms"):
            value = raw.get(name)
            if value is not None and (isinstance(value, bool) or not isinstance(value, int) or not 0 <= value <= 0xFFFFFFFF):
                raise ValueError()
            result[name] = value
        for name in ("target_temperature", "negative_delta", "positive_delta", "sensor_temperature"):
            value = raw.get(name)
            if name == "sensor_temperature" and value is None:
                result[name] = None
                continue
            if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or (name.endswith("delta") and value < 0):
                raise ValueError()
            result[name] = value
        for name in ("current_ip", "nina_firmware", "settings_source", "last_command_id"):
            if not isinstance(raw.get(name), str) or len(raw[name]) > 128:
                raise ValueError()
            result[name] = raw[name]
        configuration = raw["config"]
        if not isinstance(configuration, dict):
            raise ValueError()
        result["config"] = {}
        for name in CONFIG_FIELDS - {"password"}:
            value = configuration[name]
            if name == "api_port":
                if isinstance(value, bool) or not isinstance(value, int) or not 1 <= value <= 65535:
                    raise ValueError()
            elif not isinstance(value, str) or len(value) > 128:
                raise ValueError()
            result["config"][name] = value
        if not isinstance(configuration.get("password_set"), bool):
            raise ValueError()
        result["config"]["password_set"] = configuration["password_set"]
        return result
    except (KeyError, ValueError, TypeError, OverflowError):
        raise ControllerFailure("The controller returned an invalid status response.", 502, "controller_response_invalid") from None


def operation_key(house_id=None, room_id=None):
    return "setup" if house_id is None else hashlib.sha256(json.dumps([house_id, room_id]).encode()).hexdigest()


def public_operation(operation):
    if not operation:
        return None
    return {name: operation[name] for name in ("command_id", "kind", "state", "created_at", "device_id", "old_ip", "new_ip", "acknowledged")}


def verified(operation, status):
    if status["device_id"] != operation["device_id"] or status["last_command_id"] != operation["command_id"] or status["restart_pending"]:
        return False
    for key, value in operation.get("config", {}).items():
        if key != "password" and status["config"].get(key) != value:
            return False
    return status["current_ip"] == operation["new_ip"]


class OperationStore:
    def __init__(self, directory):
        self.directory = Path(directory)
        self.directory.mkdir(parents=True, exist_ok=True)
        self.path = self.directory / "operations.json"
        self._mutex = threading.RLock()

    @contextmanager
    def transaction(self):
        try:
            with self._mutex, portalocker.Lock(str(self.directory / ".lock"), timeout=10):
                if self.path.exists():
                    try:
                        value = json.loads(self.path.read_text(encoding="utf-8"))
                        if not isinstance(value, dict) or any(not isinstance(op, dict) or not {"command_id", "device_id", "old_ip", "new_ip", "kind", "state", "created_at", "acknowledged"} <= op.keys() for op in value.values()):
                            raise ValueError()
                    except (OSError, ValueError, TypeError):
                        raise Problem("Controller operation storage could not be read; existing files were preserved.", 503, "storage_error") from None
                else:
                    value = {}
                yield value
        except portalocker.exceptions.LockException:
            raise Problem("Controller operation storage is busy. Retry shortly.", 503, "storage_busy") from None

    def save(self, operations):
        temporary = None
        try:
            with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=self.directory, suffix=".tmp", delete=False) as stream:
                temporary = stream.name
                json.dump(operations, stream, allow_nan=False, indent=2)
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(temporary, self.path)
            if os.name != "nt":
                descriptor = os.open(self.directory, os.O_RDONLY | os.O_DIRECTORY)
                try:
                    os.fsync(descriptor)
                finally:
                    os.close(descriptor)
        except OSError:
            raise Problem("Controller operation could not be saved.", 503, "storage_error") from None
        finally:
            if temporary and os.path.exists(temporary):
                os.unlink(temporary)


def validate_reservations(houses, operations):
    """Called while holding household lock, followed by operation lock."""
    for operation in operations.values():
        target = (operation.get("house_id"), operation.get("room_id"))
        found = target[0] is None
        for house in houses.values():
            for room in house["rooms"]:
                is_target = (house["id"], room["id"]) == target
                if is_target:
                    found = True
                    desired = operation["desired_assignment"]
                    current = {key: room[key] for key in desired}
                    if current != desired:
                        raise Problem("Verify the pending controller operation before changing its assignment.", 409, "controller_operation_pending")
                elif room["controller_ip"] in (operation["old_ip"], operation["new_ip"]):
                    raise Problem("Controller IP is reserved by an unresolved operation.", 409, "ip_reserved")
        if not found:
            raise Problem("Verify the pending controller operation before removing its room.", 409, "controller_operation_pending")
