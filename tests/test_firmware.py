"""Execute the actual firmware headers compiled as a host shared library."""

import ctypes
import os
from pathlib import Path

import pytest


@pytest.fixture
def logic():
    fallback = Path(__file__).resolve().parents[1] / ".tools" / ("control.dll" if os.name == "nt" else "control.so")
    path = Path(os.getenv("BMS_FIRMWARE_TEST_LIBRARY", str(fallback)))
    if not path.exists():
        pytest.skip("Compile tests/firmware_bridge.cpp as described in README.md to run firmware logic tests.")
    library = ctypes.CDLL(str(path))
    library.logic_reading.argtypes = [ctypes.c_double, ctypes.c_uint32]
    library.logic_api.argtypes = [ctypes.c_double, ctypes.c_double, ctypes.c_double, ctypes.c_uint32]
    library.logic_sensor_changed.argtypes = [ctypes.c_uint32]
    library.logic_tick.argtypes = [ctypes.c_uint32]
    for name in ("logic_target", "logic_negative", "logic_positive"):
        getattr(library, name).restype = ctypes.c_double
    library.ruuvi_decode.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t, ctypes.POINTER(ctypes.c_double)]
    library.network_address_valid.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_int]
    library.network_config_valid.argtypes = [ctypes.POINTER(ctypes.c_uint8)] * 3
    library.recovery_reset.argtypes = [ctypes.c_uint32, ctypes.c_int]
    library.recovery_wifi.argtypes = [ctypes.c_int, ctypes.c_uint32]
    for name in ("recovery_advertisement", "recovery_arm", "recovery_due"):
        getattr(library, name).argtypes = [ctypes.c_uint32]
    library.recovery_schedule.argtypes = [ctypes.c_uint32, ctypes.c_uint32]
    library.settings_valid.argtypes = [ctypes.c_char_p] * 3 + [ctypes.c_int] + [ctypes.c_char_p] * 5
    library.settings_command_id.argtypes = [ctypes.c_char_p]
    library.http_feed.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
    library.trace_feed.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
    for name in ("http_method", "http_path", "http_body"):
        getattr(library, name).restype = ctypes.c_char_p
    library.logic_reset()
    return library


def test_startup_hysteresis_and_strict_boundaries(logic):
    assert logic.logic_tick(0) == 1
    logic.logic_api(23, 0.1, 0.1, 0)
    logic.logic_reading(23.1, 1)
    assert logic.logic_tick(1) == 1
    logic.logic_reading(23.11, 2)
    assert logic.logic_tick(2) == 0
    logic.logic_reading(22.9, 3)
    assert logic.logic_tick(3) == 0
    logic.logic_reading(22.89, 4)
    assert logic.logic_tick(4) == 1


def test_sensor_timeout_forces_on_and_recovers(logic):
    logic.logic_reading(25, 100)
    assert logic.logic_tick(100) == 0
    assert logic.logic_tick(30100) == 0
    assert logic.logic_tick(30101) == 1
    logic.logic_reading(25, 30102)
    assert logic.logic_tick(30102) == 0


def test_expired_sample_cannot_revive_after_a_full_timer_cycle(logic):
    logic.logic_reading(25, 100)
    assert logic.logic_tick(100) == 0
    assert logic.logic_tick(30101) == 1
    assert logic.logic_tick(100) == 1  # Same timestamp after a complete 32-bit cycle.


def test_five_hour_fallback_and_recovery(logic):
    logic.logic_api(19, 0.5, 0.6, 100)
    logic.logic_reading(22, 18000099)
    logic.logic_tick(18000099)
    assert logic.logic_target() == 19
    logic.logic_tick(18000100)
    assert (logic.logic_target(), logic.logic_negative(), logic.logic_positive()) == (23, 0.1, 0.1)
    logic.logic_api(21, 0.2, 0.3, 18000101)
    logic.logic_tick(18000101)
    assert logic.logic_target() == 21


def test_sensor_failure_has_priority_over_api_expiry(logic):
    logic.logic_api(20, 0.1, 0.1, 0)
    logic.logic_reading(25, 0)
    assert logic.logic_tick(1) == 0
    assert logic.logic_tick(18000000) == 1
    assert logic.logic_target() == 23


def test_timers_cross_millis_rollover(logic):
    beginning = 2**32 - 10000
    logic.logic_api(19, 0.3, 0.4, beginning)
    logic.logic_reading(25, beginning)
    assert logic.logic_tick(20000) == 0  # exactly 30 seconds after the reading
    assert logic.logic_tick(20001) == 1
    logic.logic_tick((beginning + 18000000) % 2**32)
    assert logic.logic_target() == 23


def test_reassignment_discards_old_reading_and_nan_cannot_refresh(logic):
    logic.logic_reading(25, 0)
    assert logic.logic_tick(0) == 0
    logic.logic_sensor_changed(100)
    assert logic.logic_tick(100) == 0
    logic.logic_reading(float('nan'), 30000)
    assert logic.logic_tick(30101) == 1


@pytest.mark.parametrize("raw,expected", [(0x12FC, 24.3), (0xFC18, -5.0), (0x0000, 0.0), (0x7FFF, 163.835)])
def test_ruuvi_signed_temperature_and_official_vector(logic, raw, expected):
    payload = (ctypes.c_uint8 * 26)(0x99, 0x04, 5, raw >> 8, raw & 255)
    value = ctypes.c_double()
    assert logic.ruuvi_decode(payload, 26, ctypes.byref(value)) == 1
    assert value.value == pytest.approx(expected)


def test_ruuvi_invalid_truncated_and_wrong_company_packets(logic):
    value = ctypes.c_double()
    payload = (ctypes.c_uint8 * 26)(0x99, 0x04, 5, 0x80, 0)
    assert logic.ruuvi_decode(payload, 26, ctypes.byref(value)) == 0
    payload[3] = 0
    assert logic.ruuvi_decode(payload, 4, ctypes.byref(value)) == 0
    payload[0] = 0
    assert logic.ruuvi_decode(payload, 26, ctypes.byref(value)) == 0


def ipv4_bytes(value):
    return (ctypes.c_uint8 * 4)(*map(int, value.split('.')))


def test_subnet_mask_uses_mask_rules_instead_of_host_rules(logic):
    mask = ipv4_bytes('255.255.255.0')
    assert logic.network_address_valid(mask, 1) == 1
    assert logic.network_address_valid(mask, 0) == 0
    assert logic.network_address_valid(ipv4_bytes('192.168.0.211'), 0) == 1


@pytest.mark.parametrize('prefix', range(1, 31))
def test_contiguous_masks_with_usable_host_addresses(logic, prefix):
    mask = (0xFFFFFFFF << (32 - prefix)) & 0xFFFFFFFF
    address = (ctypes.c_uint8 * 4)(mask >> 24, (mask >> 16) & 255, (mask >> 8) & 255, mask & 255)
    assert logic.network_address_valid(address, 1) == 1


@pytest.mark.parametrize('mask', ['0.0.0.0', '255.0.255.0', '255.255.255.1', '255.255.255.254', '255.255.255.255'])
def test_invalid_masks_are_rejected(logic, mask):
    assert logic.network_address_valid(ipv4_bytes(mask), 1) == 0


@pytest.mark.parametrize('address,gateway,expected', [
    ('192.168.0.211', '192.168.0.1', 1),
    ('192.168.0.211', '192.168.1.1', 0),
    ('192.168.0.0', '192.168.0.1', 0),
    ('192.168.0.255', '192.168.0.1', 0),
])
def test_static_network_checks_controller_and_gateway(logic, address, gateway, expected):
    assert logic.network_config_valid(ipv4_bytes(address), ipv4_bytes('255.255.255.0'), ipv4_bytes(gateway)) == expected


def test_recovery_wifi_needs_ten_continuous_minutes_and_recovers(logic):
    logic.recovery_reset(100, 0)
    logic.recovery_advertisement(600100)
    assert logic.recovery_due(600100) == 0
    logic.recovery_wifi(0, 600100)
    assert logic.recovery_due(600101) == 1
    logic.recovery_wifi(1, 600101)
    assert logic.recovery_due(600101) == 0
    logic.recovery_advertisement(1200101)
    assert logic.recovery_due(1200101) == 0
    assert logic.recovery_due(1200102) == 1


def test_any_advertisement_refreshes_recovery_without_a_temperature(logic):
    logic.recovery_reset(0, 0)
    logic.recovery_wifi(1, 600000)
    assert logic.recovery_due(600000) == 0
    assert logic.recovery_due(600001) == 1
    # This does not call reading(): unrelated/unsupported BLE advertisements count.
    logic.recovery_advertisement(600001)
    assert logic.recovery_due(600001) == 0
    assert logic.logic_tick(600001) == 1  # Independent missing-assigned-sensor rule.


def test_provisioning_exemption_and_explicit_restart(logic):
    logic.recovery_reset(0, 1)
    assert logic.recovery_due(3_600_000) == 0
    logic.recovery_schedule(3_600_000, 500)
    assert logic.recovery_due(3_600_499) == 0
    assert logic.recovery_due(3_600_500) == 1
    logic.recovery_reset(100, 0)
    logic.recovery_suspend()
    assert logic.recovery_due(3_600_000) == 0
    logic.recovery_arm(3_600_000)
    assert logic.recovery_due(4_200_000) == 0
    assert logic.recovery_due(4_200_001) == 1


def test_recovery_deadlines_survive_millis_rollover(logic):
    start = 2**32 - 100_000
    logic.recovery_reset(start, 0)
    assert logic.recovery_due(500_000) == 0
    assert logic.recovery_due(500_001) == 1
    logic.recovery_reset(start, 1)
    logic.recovery_schedule(2**32 - 250, 500)
    assert logic.recovery_due(249) == 0
    assert logic.recovery_due(250) == 1


def controller_settings(logic, **changes):
    values = dict(ssid='Home WiFi', password='password123', host='BMS', port=2026,
                  mac='', ip='192.168.0.230', mask='255.255.255.0', gateway='192.168.0.1', dns='192.168.0.1')
    values.update(changes)
    return logic.settings_valid(*(value if isinstance(value, int) else value.encode() for value in values.values()))


def test_settings_allow_initial_unassigned_sensor_and_open_wifi(logic):
    assert controller_settings(logic) == 1
    assert controller_settings(logic, password='', mac='aa:BB:cc:DD:ee:FF') == 1
    assert controller_settings(logic, ip='10.0.0.23', mask='255.0.0.0', gateway='10.0.0.1', dns='1.1.1.1') == 1


@pytest.mark.parametrize('changes', [
    {'ssid': ''}, {'ssid': 'x' * 33}, {'ssid': 'bad\nname'}, {'password': 'short'},
    {'password': 'x' * 64}, {'password': 'bad\npassword'}, {'host': 'http://BMS'},
    {'host': ''}, {'port': 0}, {'port': 65536}, {'mac': 'invalid'},
    {'ip': '192.168.0.0'}, {'ip': '192.168.0.255'}, {'ip': '224.0.0.2'},
    {'ip': '127.0.0.2'}, {'ip': '192.168.0.999'}, {'ip': '192.168.0.2x'},
    {'ip': '192.168.00.2'},
    {'mask': '255.0.255.0'}, {'gateway': '192.168.1.1'},
    {'gateway': '192.168.0.0'}, {'gateway': '192.168.0.255'},
    {'gateway': '192.168.0.230'}, {'dns': '0.0.0.0'},
])
def test_settings_reject_invalid_candidate(logic, changes):
    assert controller_settings(logic, **changes) == 0


def test_command_ids_are_bounded_uuids(logic):
    assert logic.settings_command_id(b'550e8400-e29b-41d4-a716-446655440000') == 1
    for value in (b'', b'restart', b'x' * 36, b'550e8400-e29b-41d4-a716-4466554400000'):
        assert logic.settings_command_id(value) == 0


def http_feed(logic, data):
    return logic.http_feed(data, len(data))


def test_http_parses_fragmented_headers_and_complete_json_body(logic):
    logic.http_reset()
    assert http_feed(logic, b'POST /config HTTP/1.1\r\nHost: controller\r\nContent-Type: application/json\r\nContent-Length: 2\r\n\r\n{') == 0
    assert http_feed(logic, b'}') == 1
    assert logic.http_method() == b'POST'
    assert logic.http_path() == b'/config'
    assert logic.http_body() == b'{}'
    logic.http_reset()
    assert http_feed(logic, b'GET /status HTTP/1.0\r\n\r\n') == 1


@pytest.mark.parametrize('raw_request', [
    b'POST /config HTTP/1.1\r\nContent-Type: application/json\r\n\r\n{}',
    b'POST /config HTTP/1.1\r\nContent-Type: text/plain\r\nContent-Length: 2\r\n\r\n{}',
    b'POST /config HTTP/1.1\r\nContent-Type: application/json\r\nContent-Length: 2\r\nContent-Length: 3\r\n\r\n{}',
    b'POST /config HTTP/1.1\r\nContent-Type: application/json\r\nContent-Length: 2x\r\n\r\n{}',
    b'POST /config HTTP/1.1\r\nContent-Type: application/json\r\nContent-Length: 1025\r\n\r\n',
    b'POST /config HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n',
    b'GET /status HTTP/2\r\n\r\n', b'GET /status HTTP/1.1\r\n' + b'x' * 1024,
    b'GET /status HTTP/1.1\x00\r\n\r\n',
    b'GET /status HTTP/1.1\n\n',
    b'GET /status HTTP/1.1\r\nBad header: value\r\n\r\n',
])
def test_http_rejects_ambiguous_or_oversized_requests(logic, raw_request):
    logic.http_reset()
    assert http_feed(logic, raw_request) == -400


def trace_line(packet, prefix=b'HCI EVENT RX <- '):
    return prefix + packet.hex().upper().encode() + b'\r\n'


def test_raw_ble_observer_counts_fragmented_advertisement_before_discovery(logic):
    logic.trace_reset()
    # Connectable advertisement, empty data, no scan response: BLEDiscovered is not emitted.
    packet = bytes([4, 0x3E, 12, 2, 1, 0, 0, 1, 2, 3, 4, 5, 6, 0, 0xC4])
    line = trace_line(packet)
    assert logic.trace_feed(line[:11], 11) == 0
    assert logic.trace_feed(line[11:-1], len(line) - 12) == 0
    assert logic.trace_feed(line[-1:], 1) == 1
    assert logic.trace_feed(line, len(line)) == 1  # Duplicate advertisements also count.


@pytest.mark.parametrize('line', [
    trace_line(bytes([4, 0x0E, 4, 1, 3, 0x0C, 0])),  # Command-complete event.
    trace_line(bytes([4, 0x3E, 2, 1, 1])),  # Other LE subevent.
    trace_line(bytes([4, 0x3E, 2, 2, 0])),  # Zero advertising reports.
    trace_line(bytes([4, 0x3E, 12, 2, 1])),  # Truncated report.
    trace_line(bytes([4, 0x3E, 2, 2, 1]), b'HCI COMMAND TX -> '),
    b'HCI EVENT RX <- 043E02020Z\r\n',
])
def test_raw_ble_observer_ignores_unrelated_and_incomplete_events(logic, line):
    logic.trace_reset()
    assert logic.trace_feed(line, len(line)) == 0
    valid = trace_line(bytes([4, 0x3E, 12, 2, 1, 0, 0, 1, 2, 3, 4, 5, 6, 0, 0xC4]))
    assert logic.trace_feed(valid, len(valid)) == 1
