import copy
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
from zoneinfo import ZoneInfo

import pytest

from backend.api import create_app
from backend.scheduler import active_target


@pytest.fixture
def app(tmp_path):
    return create_app({"TESTING": True, "DATA_DIR": str(tmp_path / "houses"), "DEFAULT_TIMEZONE": "Europe/Bucharest", "NOW": lambda: datetime(2026, 10, 10, 12, tzinfo=timezone.utc)})


@pytest.fixture
def house():
    return {"name": "Main house", "timezone": "Europe/Bucharest", "rooms": [
        {"id": "living", "name": "Living room", "controller_ip": "192.168.1.50", "sensor_mac": "aa:bb:cc:dd:ee:ff", "negative_delta": 0.2, "positive_delta": 0.3, "programs": [
            {"id": "morning", "start_time": "07:00", "target_temperature": 22},
            {"id": "evening", "start_time": "21:00", "target_temperature": 19},
            {"id": "weekend", "start_time": "07:00", "days": ["saturday", "sunday"], "target_temperature": 24},
        ]},
    ]}


def put(client, value, identifier="home"):
    return client.put("/set_config", query_string={"id": identifier}, json=value)


def test_household_persistence_and_source_ip(app, house):
    with app.test_client() as client:
        assert client.get("/get_house_list").json == []
        result = put(client, house)
        assert result.status_code == 201
        assert result.json["rooms"][0]["sensor_mac"] == "AA:BB:CC:DD:EE:FF"
        assert put(client, house).status_code == 200
        assert client.get("/get_house_list").json == [{"id": "home", "name": "Main house"}]
        state = client.get("/get_state", environ_overrides={"REMOTE_ADDR": "192.168.1.50"})
        assert state.json == {"target_temperature": 24, "negative_delta": 0.2, "positive_delta": 0.3, "sensor_mac": "AA:BB:CC:DD:EE:FF"}
        assert client.get("/get_state", headers={"X-Forwarded-For": "192.168.1.50"}).status_code == 404
    restarted = create_app({"TESTING": True, "DATA_DIR": app.config["DATA_DIR"]})
    assert restarted.test_client().get("/get_config?id=home").json["rooms"] == result.json["rooms"]


@pytest.mark.parametrize("mutate", [
    lambda h: h.pop("rooms"),
    lambda h: h["rooms"][0].pop("programs"),
    lambda h: h["rooms"][0].update(negative_delta=-1),
    lambda h: h["rooms"][0].update(positive_delta=True),
    lambda h: h["rooms"][0].update(sensor_mac="bad"),
    lambda h: h["rooms"][0].update(controller_ip="999.1.1.1"),
    lambda h: h["rooms"][0].update(controller_ip=1234),
    lambda h: h.update(timezone="Unknown/Timezone"),
    lambda h: h["rooms"][0]["programs"][0].update(start_time="24:00"),
    lambda h: h["rooms"][0]["programs"][0].update(target_temperature=float("nan")),
    lambda h: h["rooms"][0]["programs"][0].update(days=[]),
    lambda h: h["rooms"][0]["programs"][0].update(days=["monday", "monday"]),
    lambda h: h["rooms"][0]["programs"].append({"id": "collision", "days": ["saturday"], "start_time": "07:00", "target_temperature": 20}),
])
def test_invalid_replacement_is_atomic(app, house, mutate):
    client = app.test_client()
    assert put(client, house).status_code == 201
    before = client.get("/get_config?id=home").json
    invalid = copy.deepcopy(house)
    mutate(invalid)
    response = put(client, invalid)
    assert response.status_code == 400
    assert response.json["error"]["message"]
    assert client.get("/get_config?id=home").json == before


def test_global_ip_conflict_and_full_replacement(app, house):
    client = app.test_client()
    put(client, house)
    assert put(client, house, "second").status_code == 409
    assert client.get("/get_house_list").json == [{"id": "home", "name": "Main house"}]
    replacement = {"name": "Main house", "rooms": []}
    assert put(client, replacement).status_code == 200
    assert client.get("/get_state", environ_overrides={"REMOTE_ADDR": "192.168.1.50"}).status_code == 404
    assert client.get("/get_config?id=home").json["timezone"] == "Europe/Bucharest"


def test_resource_operations_and_patch_preservation(app, house):
    client = app.test_client()
    put(client, house)
    base = "/households/home/rooms/living"
    before = client.get("/get_config?id=home").json
    patched = client.patch(base, json={"positive_delta": 0.7})
    assert patched.status_code == 200
    assert patched.json["programs"] == before["rooms"][0]["programs"]
    assert client.patch(base, json={"programs": []}).status_code == 400
    assert client.patch(base, json={"positive_delta": None}).status_code == 400
    assert client.patch(base, json={}).status_code == 400
    p = client.patch(base + "/programs/weekend", json={"target_temperature": 25})
    assert p.json["days"] == ["saturday", "sunday"] and p.json["start_time"] == "07:00"
    assert client.patch(base + "/programs/weekend", json={"days": list(("monday", "tuesday", "wednesday", "thursday", "friday", "saturday", "sunday"))}).status_code == 400
    created = client.post(base + "/programs", json={"id": "lunch", "start_time": "12:00", "target_temperature": 21})
    assert created.status_code == 201
    assert client.post(base + "/programs", json=created.json).status_code == 409
    assert client.delete(base + "/programs/lunch").status_code == 204
    assert client.delete(base + "/programs/lunch").status_code == 404
    room = {"id": "bed", "name": "Bedroom", "controller_ip": "192.168.1.51", "sensor_mac": "11:22:33:44:55:66"}
    result = client.post("/households/home/rooms", json=room)
    assert result.status_code == 201 and result.json["programs"] == [] and result.json["negative_delta"] == 0.1
    assert client.delete("/households/home/rooms/bed").status_code == 204
    assert len(client.get("/get_config?id=home").json["rooms"]) == 1


def test_concurrent_changes_do_not_drop_other_fields(app, house):
    put(app.test_client(), house)
    def update(change):
        with app.test_client() as client:
            return client.patch("/households/home/rooms/living", json=change).status_code
    with ThreadPoolExecutor(max_workers=2) as pool:
        assert list(pool.map(update, [{"negative_delta": 0.8}, {"positive_delta": 0.9}])) == [200, 200]
    room = app.test_client().get("/get_config?id=home").json["rooms"][0]
    assert (room["negative_delta"], room["positive_delta"]) == (0.8, 0.9)


def test_corrupt_storage_is_reported_and_preserved(app, house):
    put(app.test_client(), house)
    path = next(app.extensions["house_store"].directory.glob("*.json"))
    path.write_text("broken", encoding="utf-8")
    assert app.test_client().get("/get_house_list").status_code == 503
    assert put(app.test_client(), house).status_code == 503
    assert path.read_text() == "broken"


def test_local_access_cors_and_errors(app):
    client = app.test_client()
    assert client.get("/get_house_list", environ_overrides={"REMOTE_ADDR": "8.8.8.8"}).status_code == 403
    assert client.get("/get_house_list", headers={"Origin": "https://evil.example"}).status_code == 403
    response = client.options("/set_config", headers={"Origin": "http://192.168.1.12:3000", "Access-Control-Request-Method": "PUT"})
    assert response.status_code == 200
    assert response.headers["Access-Control-Allow-Origin"] == "http://192.168.1.12:3000"
    assert client.get("/get_config").status_code == 400
    assert client.get("/get_config?id=absent").status_code == 404
    assert client.put("/set_config?id=home", data="{", content_type="application/json").status_code == 400
    assert client.put("/set_config?id=home", json=[]).status_code == 400


def test_scheduler_boundaries_and_weekly_spillover(house):
    room = house["rooms"][0]
    assert active_target(room, datetime(2026, 10, 10, 6, 59, tzinfo=timezone.utc)) == 19
    assert active_target(room, datetime(2026, 10, 10, 7, tzinfo=timezone.utc)) == 24
    assert active_target(room, datetime(2026, 10, 12, 7, tzinfo=timezone.utc)) == 22
    room["programs"] = [{"id": "weekly", "start_time": "20:00", "days": ["monday"], "target_temperature": 18}]
    assert active_target(room, datetime(2026, 10, 12, 19, tzinfo=timezone.utc)) == 18
    assert active_target({"programs": []}) == 23


def test_dst_never_repeats_or_rewrites_utc_schedule():
    room = {"programs": [{"id": "early", "start_time": "00:15", "target_temperature": 22}, {"id": "later", "start_time": "01:15", "target_temperature": 19}]}
    zone = ZoneInfo("Europe/Bucharest")
    # The two 03:30 wall-clock readings are different UTC instants.
    first = datetime(2026, 10, 25, 3, 30, tzinfo=zone, fold=0)
    second = datetime(2026, 10, 25, 3, 30, tzinfo=zone, fold=1)
    assert active_target(room, first) == 22
    assert active_target(room, second) == 19
    assert room["programs"][0]["start_time"] == "00:15"
