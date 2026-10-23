import copy
import json
from datetime import datetime, timezone

import pytest

from backend.api import create_app
from backend.controllers import ControllerFailure, ControllerTransport

BASE = "/households/home/rooms/living"
DEVICE = "0123456789abcdef0123456789abcdef"
IP = "192.168.0.211"
NEW_IP = "192.168.0.212"


def device_status(address=IP):
    return {
        "device_id": DEVICE, "uptime_ms": 100000, "wifi_connected": True, "wifi_status": 3,
        "current_ip": address, "ble_scanning": True, "nina_firmware": "3.0.1", "heater_on": False,
        "target_temperature": 23, "negative_delta": .1, "positive_delta": .1, "settings_source": "api",
        "api_age_ms": 100, "sensor_temperature": 24.125, "sensor_age_ms": 50, "ble_age_ms": 10,
        "setup_mode": False, "restart_pending": False, "last_command_id": "",
        "config": {"ssid": "Home", "password_set": True, "ip": address, "subnet": "255.255.255.0",
                   "gateway": "192.168.0.1", "dns": "192.168.0.1", "api_host": "BMS", "api_port": 2026,
                   "sensor_mac": "AA:BB:CC:DD:EE:FF", "password": "never-return-this"},
        "password": "never-return-this-either",
    }


class FakeTransport:
    def __init__(self):
        self.devices = {IP: device_status()}
        self.calls = []
        self.delivery_failure = None
        self.status_failure = None
        self.last_payload = None

    def request(self, address, method, path, payload=None):
        self.calls.append((address, method, path, copy.deepcopy(payload)))
        if method == "GET":
            if self.status_failure:
                raise self.status_failure
            if address not in self.devices:
                raise ControllerFailure("Not connected.")
            return copy.deepcopy(self.devices[address])
        self.last_payload = copy.deepcopy(payload)
        if self.delivery_failure:
            raise self.delivery_failure
        return {"accepted": True, "command_id": payload["command_id"], "restart_required": True}

    def complete(self):
        payload = self.last_payload
        value = copy.deepcopy(next(iter(self.devices.values())))
        patch = payload.get("config", {})
        value["config"].update(patch)
        if "password" in patch:
            value["config"]["password_set"] = bool(patch["password"])
        address = value["config"]["ip"]
        value.update(current_ip=address, last_command_id=payload["command_id"], uptime_ms=500, restart_pending=False)
        self.devices = {address: value}


@pytest.fixture
def transport():
    return FakeTransport()


@pytest.fixture
def app(tmp_path, transport):
    value = create_app({"TESTING": True, "DATA_DIR": str(tmp_path / "houses"), "CONTROLLER_TRANSPORT": transport,
                        "DEFAULT_TIMEZONE": "UTC", "NOW": lambda: datetime(2026, 10, 23, tzinfo=timezone.utc)})
    result = value.test_client().put("/set_config?id=home", json={"name": "Home", "rooms": [
        {"id": "living", "name": "Living room", "controller_ip": IP, "sensor_mac": "AA:BB:CC:DD:EE:FF", "programs": []}
    ]})
    assert result.status_code == 201
    return value


def test_status_is_live_whitelisted_and_offline_is_honest(app, transport):
    client = app.test_client()
    response = client.get(BASE + "/controller/status")
    assert response.status_code == 200
    assert response.json["reachable"] is True
    assert response.json["status"]["sensor_temperature"] == 24.125
    assert "password" not in response.json["status"]["config"]
    assert "never-return" not in response.get_data(as_text=True)
    transport.status_failure = ControllerFailure("Not connected.")
    response = client.get(BASE + "/controller/status")
    assert response.json["reachable"] is False and response.json["status"] is None
    assert client.get("/households/home/rooms/missing/controller/status").status_code == 404


@pytest.mark.parametrize("patch", [
    {}, {"unknown": 1}, {"password": "short"}, {"password": None}, {"api_port": True},
    {"api_port": 65536}, {"api_host": "http://remote/path"}, {"ip": "192.168.0.0"},
    {"gateway": "192.168.1.1"}, {"gateway": "192.168.0.0"}, {"gateway": "192.168.0.255"}, {"gateway": IP},
    {"subnet": "255.0.255.0"}, {"sensor_mac": ""}, {"ssid": ""}, {"ssid": "\ud800"},
    {"password": "\ud800abcdefgh"},
    {"password": "parolă123"}, {"password": "password\t"}, {"ssid": "Wi\tFi"}, {"ssid": "Wi\x7fFi"},
    {"ip": "0.168.0.10"}, {"ip": "224.168.0.10"}, {"ip": "127.0.0.10"}, {"dns": "127.0.0.1"},
])
def test_invalid_remote_config_does_not_deliver_or_change_assignment(app, transport, patch):
    client = app.test_client()
    before = client.get("/get_config?id=home").json
    response = client.patch(BASE + "/controller/config", json=patch)
    assert response.status_code == 400
    assert not any(call[1] == "POST" for call in transport.calls)
    assert client.get("/get_config?id=home").json == before


def test_config_handover_updates_server_before_delivery_and_verifies_device(app, transport):
    client = app.test_client()
    result = client.patch(BASE + "/controller/config", json={"ip": NEW_IP, "sensor_mac": "11:22:33:44:55:66", "password": "changed-secret"})
    assert result.status_code == 202 and result.json["confirmed"] is False
    assert "changed-secret" not in result.get_data(as_text=True)
    room = client.get("/get_config?id=home").json["rooms"][0]
    assert (room["controller_ip"], room["sensor_mac"]) == (NEW_IP, "11:22:33:44:55:66")
    assert transport.calls[-1][:3] == (IP, "POST", "/config")
    assert client.get("/get_state", environ_overrides={"REMOTE_ADDR": NEW_IP}).json["sensor_mac"] == "11:22:33:44:55:66"
    assert client.patch(BASE, json={"name": "Renamed"}).status_code == 200
    assert client.patch(BASE, json={"sensor_mac": "66:55:44:33:22:11"}).status_code == 409
    assert client.delete(BASE).status_code == 409
    assert client.post(BASE + "/controller/restart", json={}).status_code == 409
    transport.complete()
    response = client.get(BASE + "/controller/status")
    assert response.json["pending"] is None
    assert response.json["confirmed_command_id"] == result.json["command_id"]
    assert "changed-secret" not in app.extensions["controller_operations"].path.read_text()
    assert client.get("/get_config?id=home").json["rooms"][0]["name"] == "Renamed"


def test_explicit_rejection_rolls_back_only_assignment_fields(app, transport):
    client = app.test_client()
    original_request = transport.request

    def reject_after_concurrent_edit(address, method, path, payload=None):
        if method == "POST":
            assert client.patch(BASE, json={"name": "Concurrent rename"}).status_code == 200
            assert client.post(BASE + "/programs", json={"id": "evening", "start_time": "21:00", "target_temperature": 19}).status_code == 201
            assert client.patch(BASE, json={"controller_ip": IP, "sensor_mac": "AA:BB:CC:DD:EE:FF"}).status_code == 409
            raise ControllerFailure("Controller rejected.", 400, "controller_rejected", True)
        return original_request(address, method, path, payload)

    transport.request = reject_after_concurrent_edit
    response = client.patch(BASE + "/controller/config", json={"ip": NEW_IP, "sensor_mac": "11:22:33:44:55:66"})
    assert response.status_code == 400 and response.json["pending"] is None
    assigned = client.get("/get_config?id=home").json["rooms"][0]
    assert assigned["controller_ip"] == IP and assigned["sensor_mac"] == "AA:BB:CC:DD:EE:FF"
    assert assigned["name"] == "Concurrent rename" and assigned["programs"][0]["id"] == "evening"
    assert json.loads(app.extensions["controller_operations"].path.read_text()) == {}


def test_timeout_keeps_reservations_across_api_restart_and_retry_reuses_id(app, transport):
    client = app.test_client()
    transport.delivery_failure = ControllerFailure("Response timed out.", 504, "controller_timeout")
    response = client.patch(BASE + "/controller/config", json={"ip": NEW_IP, "password": "keep-for-retry"})
    assert response.status_code == 504 and response.json["pending"]["state"] == "unconfirmed"
    command_id = response.json["pending"]["command_id"]
    restarted = create_app({"TESTING": True, "DATA_DIR": app.config["DATA_DIR"], "CONTROLLER_TRANSPORT": transport})
    client = restarted.test_client()
    for address in (IP, NEW_IP):
        response = client.post("/households/home/rooms", json={"id": "another", "name": "Another", "controller_ip": address, "sensor_mac": "11:22:33:44:55:66"})
        assert response.status_code == 409
    complete = client.get("/get_config?id=home").json
    complete["rooms"] = []
    assert client.put("/set_config?id=home", json=complete).status_code == 409
    transport.delivery_failure = None
    response = client.post(BASE + "/controller/retry", json={})
    assert response.status_code == 202
    assert transport.last_payload["command_id"] == command_id
    assert transport.last_payload["config"]["password"] == "keep-for-retry"
    transport.complete()
    assert client.get(BASE + "/controller/status").json["confirmed_command_id"] == command_id


def test_restart_requires_persisted_command_proof_and_does_not_retry_automatically(app, transport):
    client = app.test_client()
    response = client.post(BASE + "/controller/restart", json={})
    command_id = response.json["command_id"]
    for _ in range(2):
        assert client.get(BASE + "/controller/status").json["pending"]["command_id"] == command_id
    assert sum(call[1] == "POST" for call in transport.calls) == 1
    transport.devices[IP]["last_command_id"] = command_id
    transport.devices[IP]["restart_pending"] = True
    assert client.post(BASE + "/controller/retry", json={}).status_code == 202
    assert sum(call[1] == "POST" for call in transport.calls) == 1
    transport.devices[IP]["restart_pending"] = False
    response = client.post(BASE + "/controller/retry", json={})
    assert response.status_code == 200 and response.json["confirmed"] is True


def test_wrong_device_at_new_ip_cannot_confirm_or_receive_retry(app, transport):
    client = app.test_client()
    result = client.patch(BASE + "/controller/config", json={"ip": NEW_IP})
    wrong = device_status(NEW_IP)
    wrong["device_id"] = "f" * 32
    wrong["last_command_id"] = result.json["command_id"]
    transport.devices = {NEW_IP: wrong}
    response = client.get(BASE + "/controller/status")
    assert response.json["reachable"] is False and response.json["pending"] is not None
    response = client.post(BASE + "/controller/retry", json={})
    assert response.status_code in (409, 502)
    assert sum(call[1] == "POST" for call in transport.calls) == 1


def test_setup_target_is_fixed_and_supports_unassigned_sensor_and_open_wifi(app, transport):
    client = app.test_client()
    setup_ip = "192.168.0.230"
    transport.devices[setup_ip] = device_status(setup_ip)
    response = client.patch("/controllers/setup/config", json={"ssid": "Setup network", "password": "", "sensor_mac": ""})
    assert response.status_code == 202
    assert transport.calls[-1][0] == setup_ip
    assert transport.last_payload["config"]["password"] == ""
    assert client.patch("/controllers/setup/config", json={"url": "http://elsewhere"}).status_code == 400


def test_setup_mutation_cannot_change_an_assigned_controller(app, transport):
    client = app.test_client()
    setup_ip = "192.168.0.230"
    transport.devices[setup_ip] = device_status(setup_ip)
    assert client.patch(BASE, json={"controller_ip": setup_ip}).status_code == 200
    response = client.patch("/controllers/setup/config", json={"sensor_mac": "11:22:33:44:55:66"})
    assert response.status_code == 409
    assert not any(call[1] == "POST" for call in transport.calls)
    assert client.get("/get_config?id=home").json["rooms"][0]["sensor_mac"] == "AA:BB:CC:DD:EE:FF"


def test_unexpected_status_ip_does_not_verify_or_redirect_proxy(app, transport):
    transport.devices[IP]["current_ip"] = "192.168.0.99"
    response = app.test_client().get(BASE + "/controller/status")
    assert response.json["reachable"] is False
    assert response.json["error"]["code"] == "controller_identity_mismatch"
    assert len(transport.calls) == 1


def test_saved_public_ip_is_not_contacted_by_management_proxy(app, transport):
    client = app.test_client()
    assert client.patch(BASE, json={"controller_ip": "8.8.8.8"}).status_code == 200
    response = client.get(BASE + "/controller/status")
    assert response.json["reachable"] is False and response.json["error"]["code"] == "local_only"
    assert transport.calls == []


def test_unreachable_preflight_does_not_reserve_or_mutate(app, transport):
    transport.status_failure = ControllerFailure("No response.", 504, "controller_timeout")
    client = app.test_client()
    assert client.patch(BASE + "/controller/config", json={"ip": NEW_IP}).status_code == 504
    assert client.get("/get_config?id=home").json["rooms"][0]["controller_ip"] == IP
    assert not app.extensions["controller_operations"].path.exists()


def test_transport_never_follows_redirects_or_leaks_upstream_error(monkeypatch):
    class Response:
        status = 302

        def read(self, size):
            return b'{"password":"secret"}'

    class Connection:
        calls = []

        def __init__(self, host, port, timeout):
            self.calls.append((host, port, timeout))

        def request(self, *args):
            pass

        def getresponse(self):
            return Response()

        def close(self):
            pass

    monkeypatch.setattr("backend.controllers.http.client.HTTPConnection", Connection)
    with pytest.raises(ControllerFailure) as failure:
        ControllerTransport().request(IP, "GET", "/status")
    assert failure.value.code == "controller_response_invalid"
    assert "secret" not in str(failure.value)
    assert Connection.calls == [(IP, 80, 15)]
