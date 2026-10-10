"""Configuration validation shared by every mutation route."""

import copy
import math
import re
from ipaddress import IPv4Address
from zoneinfo import ZoneInfo, ZoneInfoNotFoundError

DAYS = ("monday", "tuesday", "wednesday", "thursday", "friday", "saturday", "sunday")
ROOM_FIELDS = {"name", "controller_ip", "sensor_mac", "negative_delta", "positive_delta"}
PROGRAM_FIELDS = {"start_time", "target_temperature", "days"}


class Problem(Exception):
    def __init__(self, message, status=400, code="invalid_config"):
        super().__init__(message)
        self.status, self.code = status, code


def text(value, label, limit=128):
    if not isinstance(value, str) or not value.strip() or len(value) > limit:
        raise Problem(f"{label} must be a nonempty string of at most {limit} characters.")
    return value.strip()


def number(value, label, nonnegative=False):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise Problem(f"{label} must be a finite number.")
    try:
        valid = math.isfinite(value)
    except (OverflowError, TypeError):
        valid = False
    if not valid or (nonnegative and value < 0):
        raise Problem(f"{label} must be a finite {'nonnegative ' if nonnegative else ''}number.")
    return float(value)


def fields(value, allowed, label):
    if not isinstance(value, dict):
        raise Problem(f"{label} must be a JSON object.")
    unknown = set(value) - allowed
    if unknown:
        raise Problem(f"Unknown {label} fields: {', '.join(sorted(unknown))}.")


def patch(value, allowed):
    fields(value, allowed, "update")
    if not value or any(item is None for item in value.values()):
        raise Problem("An update must supply at least one field; null values are not allowed.")
    return value


def program(raw):
    fields(raw, {"id"} | PROGRAM_FIELDS, "program")
    result = {
        "id": text(raw.get("id"), "Program ID"),
        "start_time": raw.get("start_time"),
        "target_temperature": number(raw.get("target_temperature"), "Target temperature"),
    }
    if not isinstance(result["start_time"], str) or not re.fullmatch(r"(?:[01]\d|2[0-3]):[0-5]\d", result["start_time"]):
        raise Problem("A program start time must use valid UTC HH:MM format.")
    if "days" in raw:
        days = raw["days"]
        if not isinstance(days, list) or not days or any(not isinstance(d, str) or d not in DAYS for d in days):
            raise Problem("Program days must be a nonempty array of lowercase UTC weekday names.")
        if len(set(days)) != len(days):
            raise Problem("Program days must not contain duplicates.")
        if len(days) < 7:
            result["days"] = [d for d in DAYS if d in days]
    return result


def room(raw, require_programs=True):
    fields(raw, {"id", "programs"} | ROOM_FIELDS, "room")
    try:
        if not isinstance(raw.get("controller_ip"), str):
            raise ValueError("IP must be a string")
        address = str(IPv4Address(raw.get("controller_ip")))
    except (ValueError, TypeError):
        raise Problem("Controller IP must be a valid IPv4 address.") from None
    mac = raw.get("sensor_mac")
    if not isinstance(mac, str) or not re.fullmatch(r"(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}", mac):
        raise Problem("Sensor MAC must contain six hexadecimal octets separated by colons.")
    if require_programs and "programs" not in raw:
        raise Problem("Each room in a complete configuration must include its programs array.")
    programs = raw.get("programs", [])
    if not isinstance(programs, list):
        raise Problem("Room programs must be an array.")
    result = {
        "id": text(raw.get("id"), "Room ID"),
        "name": text(raw.get("name"), "Room name"),
        "controller_ip": address,
        "sensor_mac": mac.upper(),
        "negative_delta": number(raw.get("negative_delta", 0.1), "Negative delta", True),
        "positive_delta": number(raw.get("positive_delta", 0.1), "Positive delta", True),
        "programs": [program(p) for p in programs],
    }
    unique_ids(result["programs"], "program")
    for index, first in enumerate(result["programs"]):
        for second in result["programs"][index + 1:]:
            if first["start_time"] != second["start_time"]:
                continue
            first_daily, second_daily = "days" not in first, "days" not in second
            if first_daily and second_daily:
                raise Problem("Everyday programs cannot share a start time.")
            if not first_daily and not second_daily and set(first["days"]) & set(second["days"]):
                raise Problem("Day-specific programs cannot share a start time on overlapping days.")
    return result


def unique_ids(items, label):
    identifiers = [item["id"] for item in items]
    if len(set(identifiers)) != len(identifiers):
        raise Problem(f"Duplicate {label} identifiers are not allowed.")


def household(identifier, raw, default_timezone):
    fields(raw, {"id", "name", "timezone", "rooms"}, "household")
    identifier = text(identifier, "Household ID")
    if "id" in raw and raw["id"] != identifier:
        raise Problem("Household body ID must match the requested identifier.")
    timezone = raw.get("timezone", default_timezone)
    try:
        ZoneInfo(timezone)
    except (ZoneInfoNotFoundError, ValueError, TypeError):
        raise Problem("Household timezone must be a valid IANA timezone identifier.") from None
    if "rooms" not in raw or not isinstance(raw["rooms"], list):
        raise Problem("A complete household configuration must include a rooms array.")
    result = {
        "id": identifier,
        "name": text(raw.get("name"), "Household name"),
        "timezone": timezone,
        "rooms": [room(r) for r in raw["rooms"]],
    }
    unique_ids(result["rooms"], "room")
    return result


def validate_assignments(houses):
    assigned = set()
    for house in houses.values():
        for entry in house["rooms"]:
            if entry["controller_ip"] in assigned:
                raise Problem("Controller IP is already assigned to another room.", 409, "ip_conflict")
            assigned.add(entry["controller_ip"])


def find(items, identifier, label):
    for item in items:
        if item["id"] == identifier:
            return item
    raise Problem(f"{label} was not found.", 404, "not_found")


def add_unique(items, item, label):
    if any(existing["id"] == item["id"] for existing in items):
        raise Problem(f"{label} ID already exists.", 409, "id_conflict")
    items.append(copy.deepcopy(item))
