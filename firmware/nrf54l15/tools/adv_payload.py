"""Pure BLE advertising builders; all returned bytes are complete AD structures."""
from __future__ import annotations
import struct
import uuid

MODES = {"nonconnectable": 0, "scannable": 1, "connectable": 2}


def field(ad_type: int, data: bytes) -> bytes:
    if not 1 <= ad_type <= 255 or len(data) > 254:
        raise ValueError("invalid AD type or length")
    return bytes([len(data) + 1, ad_type]) + data


def validate_ad(raw: bytes) -> None:
    if len(raw) > 31:
        raise ValueError("Legacy AdvData and ScanRspData each allow at most 31 bytes")
    pos = 0
    while pos < len(raw):
        size = raw[pos]
        if size == 0:
            if any(raw[pos:]):
                raise ValueError("nonzero bytes after AD terminator")
            return
        if pos + size + 1 > len(raw):
            raise ValueError("truncated AD structure")
        pos += size + 1


def name_response(name: str | None) -> bytes:
    raw = b"" if name is None else field(0x09, name.encode("utf-8"))
    validate_ad(raw)
    return raw


def ibeacon(identifier: str, major: int, minor: int, measured_power: int) -> bytes:
    if not 0 <= major <= 65535 or not 0 <= minor <= 65535 or not -127 <= measured_power <= 20:
        raise ValueError("invalid Major, Minor or measured RSSI at 1 metre")
    manufacturer = b"\x4c\x00\x02\x15" + uuid.UUID(identifier).bytes
    manufacturer += struct.pack(">HHb", major, minor, measured_power)
    return field(1, b"\x06") + field(0xff, manufacturer)


def service_data(identifier: str, data: bytes) -> bytes:
    text = identifier.removeprefix("0x")
    if len(text) in (4, 8):
        width = len(text) // 2
        service = int(text, 16).to_bytes(width, "little")
        ad_type = 0x16 if width == 2 else 0x20
    else:
        service = uuid.UUID(text).bytes[::-1]
        ad_type = 0x21
    raw = field(1, b"\x06") + field(ad_type, service + data)
    validate_ad(raw)
    return raw


def configuration(revision: int, enabled: bool, mode: str, interval_ms: int,
                  advertising: bytes, response: bytes = b"") -> bytes:
    if mode not in MODES or not 100 <= interval_ms <= 10240:
        raise ValueError("mode invalid or interval outside 100..10240 milliseconds")
    validate_ad(advertising)
    validate_ad(response)
    if mode == "nonconnectable" and response:
        raise ValueError("scan response requires scannable or connectable mode")
    ticks = (interval_ms * 8 + 2) // 5
    return (struct.pack("<QBBHB", revision, enabled, MODES[mode], ticks, len(advertising)) +
            advertising + bytes([len(response)]) + response)


def describe_configuration(data: bytes) -> dict:
    if len(data) < 14:
        raise ValueError("short AdvConfig")
    revision, enabled, mode, ticks, size = struct.unpack_from("<QBBHB", data)
    if size > 31 or len(data) < 14 + size:
        raise ValueError("invalid AdvConfig data")
    response_size = data[13 + size]
    if response_size > 31 or len(data) != 14 + size + response_size:
        raise ValueError("invalid AdvConfig response")
    return {"revision": revision, "enabled": bool(enabled), "mode": mode,
            "intervalMs": ticks * 0.625, "advDataHex": data[13:13 + size].hex(),
            "scanResponseHex": data[14 + size:].hex()}
