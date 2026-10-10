"""Pure UTC recurrence: no local-clock comparisons or stored-time rewrites."""

from datetime import datetime, timedelta, timezone

from .model import DAYS


def active_target(room, now=None):
    now = now or datetime.now(timezone.utc)
    if now.tzinfo is None:
        raise ValueError("Schedule evaluation requires an aware datetime.")
    now = now.astimezone(timezone.utc)
    latest = None
    target = 23.0
    for program in room["programs"]:
        hour, minute = map(int, program["start_time"].split(":"))
        for days_ago in range(8):
            date = now.date() - timedelta(days=days_ago)
            if "days" in program and DAYS[date.weekday()] not in program["days"]:
                continue
            event = datetime(date.year, date.month, date.day, hour, minute, tzinfo=timezone.utc)
            if event > now:
                continue
            key = (event, "days" in program)
            if latest is None or key > latest:
                latest, target = key, program["target_temperature"]
            break
    return target
