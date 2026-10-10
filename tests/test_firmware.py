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
