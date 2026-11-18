"""GW32 UART management probe. Install pyserial only when using a serial port.

Data frames are displayed, never ACKed: this probe is not an ESP durable spool.
Connect TX/RX and crossed RTS/CTS at 3.3 V; an adapter without flow control cannot
exercise the protocol correctly. Firmware and ESP must not drive the same pins.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import secrets
import struct
import time
import adv_payload


def crc32c(data: bytes) -> int:
    crc = 0xFFFFFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0x82F63B78 if crc & 1 else 0)
    return crc ^ 0xFFFFFFFF


def cobs_encode(raw: bytes) -> bytes:
    out = bytearray(b"\0")
    code_at, code = 0, 1
    for byte in raw:
        if byte == 0:
            out[code_at] = code
            code_at = len(out)
            out.append(0)
            code = 1
        else:
            out.append(byte)
            code += 1
            if code == 255:
                out[code_at] = code
                code_at = len(out)
                out.append(0)
                code = 1
    out[code_at] = code
    return bytes(out)


def cobs_decode(segment: bytes) -> bytes:
    out = bytearray()
    pos = 0
    while pos < len(segment):
        code = segment[pos]
        pos += 1
        if code == 0 or pos + code - 1 > len(segment):
            raise ValueError("invalid COBS")
        out.extend(segment[pos:pos + code - 1])
        pos += code - 1
        if code != 255 and pos < len(segment):
            out.append(0)
    return bytes(out)


def encode(kind: int, link: int, message: int, payload: bytes = b"") -> bytes:
    if len(payload) > 3072:
        raise ValueError("payload exceeds 3072 bytes")
    raw = struct.pack("<2sBBHQI", b"GW", 1, kind, len(payload), link, message) + payload
    return b"\0" + cobs_encode(raw + struct.pack("<I", crc32c(raw))) + b"\0"


def decode(segment: bytes) -> tuple[int, int, int, bytes]:
    if len(segment) > 3107:
        raise ValueError("oversize COBS segment")
    raw = cobs_decode(segment)
    if len(raw) < 22:
        raise ValueError("short frame")
    magic, version, kind, size, link, message = struct.unpack_from("<2sBBHQI", raw)
    if magic != b"GW" or version != 1 or size > 3072 or len(raw) != 22 + size:
        raise ValueError("invalid header/length")
    if struct.unpack_from("<I", raw, 18 + size)[0] != crc32c(raw[:-4]):
        raise ValueError("CRC mismatch")
    return kind, link, message, raw[18:-4]


class Probe:
    def __init__(self, port: str):
        try:
            import serial
        except ImportError as exc:
            raise SystemExit("Install the optional dependency: python -m pip install pyserial") from exc
        self.port = serial.Serial(port, 1000000, bytesize=8, parity="N", stopbits=1,
                                  timeout=0.05, write_timeout=0.2, rtscts=True)
        self.link = secrets.randbits(64) or 1
        self.message = 0
        self.segment = bytearray()
        self.discard = False
        self.seen_batches = set()
        self.capabilities = 0
        self.response_timeout = 0.75

    def next_frame(self, deadline: float):
        while time.monotonic() < deadline:
            chunk = self.port.read(1)
            if not chunk:
                continue
            byte = chunk[0]
            if byte:
                if not self.discard:
                    self.segment.append(byte)
                    if len(self.segment) > 3107:
                        self.segment.clear()
                        self.discard = True
                continue
            if self.discard:
                self.discard = False
                continue
            segment, self.segment = bytes(self.segment), bytearray()
            if not segment:
                continue
            try:
                frame = decode(segment)
            except ValueError:
                continue
            if frame[1] == self.link:
                return frame
        return None

    def exchange(self, kind: int, response: int, payload: bytes, message: int):
        wire = encode(kind, self.link, message, payload)
        for _ in range(3):
            if self.port.write(wire) != len(wire):
                raise OSError("short serial write")
            deadline = time.monotonic() + self.response_timeout
            while time.monotonic() < deadline:
                frame = self.next_frame(deadline)
                if frame is None:
                    break
                if frame[0] == response and frame[2] == message:
                    return frame[3]
                self.show_batch(frame)
        raise TimeoutError("No response; verify power, RF diagnostic mode and crossed RTS/CTS")

    def hello(self):
        payload = self.exchange(1, 0x81, b"", 0)
        if len(payload) < 22 or len(payload) != 21 + payload[20]:
            raise ValueError("invalid HELLO_RSP")
        self.capabilities = struct.unpack_from("<I", payload, 16)[0]
        print(json.dumps({"bootId": payload[:16].hex(),
                          "capabilities": hex(self.capabilities),
                          "firmware": payload[21:].decode("utf-8")}, ensure_ascii=False))

    def command(self, opcode: int, args: bytes = b"") -> bytes:
        self.message += 1
        payload = self.exchange(0x10, 0x90, bytes([opcode]) + args, self.message)
        if len(payload) < 2 or payload[0] != opcode:
            raise ValueError("invalid CMD_RSP")
        if payload[1]:
            raise RuntimeError(f"opcode 0x{opcode:02x} status={payload[1]} (see UART protocol)")
        return payload[2:]

    def show_batch(self, frame):
        kind, _, message, payload = frame
        if kind not in (0x20, 0x21, 0x22) or message in self.seen_batches:
            return
        self.seen_batches.add(message)
        print(json.dumps({"batchType": hex(kind), "batchSequence": message,
                          "payloadHex": payload.hex(), "custodyAcknowledged": False}))


def advertising_command(probe: Probe, args):
    data = probe.command(8)
    current = adv_payload.describe_configuration(data)
    if args.adv_action == "status":
        print(json.dumps(current))
        if probe.capabilities & (1 << 10):
            state = probe.command(0x0c)
            if len(state) != 35:
                raise ValueError("invalid AdvStatus")
            print(json.dumps({"state": state[9], "lastDriverError": struct.unpack_from("<i", state, 15)[0],
                              "lastApplyMs": struct.unpack_from("<Q", state, 19)[0],
                              "liveUpdates": struct.unpack_from("<I", state, 27)[0],
                              "startCount": struct.unpack_from("<I", state, 31)[0]}))
        return
    revision = current["revision"] + 1
    if args.adv_action in ("on", "off"):
        enabled = args.adv_action == "on"
        if probe.capabilities & (1 << 10):
            result = probe.command(0x0b, struct.pack("<QB", revision, enabled))
        else:
            replacement = bytearray(data)
            struct.pack_into("<QB", replacement, 0, revision, enabled)
            result = probe.command(7, replacement)
    else:
        response = adv_payload.name_response(args.name)
        if args.adv_action == "raw":
            advertising = bytes.fromhex(args.adv_hex)
            if args.response_hex:
                if response:
                    raise ValueError("choose --name or --response-hex")
                response = bytes.fromhex(args.response_hex)
        elif args.adv_action == "ibeacon":
            advertising = adv_payload.ibeacon(args.uuid, args.major, args.minor, args.measured_power)
        else:
            advertising = adv_payload.service_data(args.uuid, bytes.fromhex(args.data_hex))
        if args.mode == "connectable" and not (probe.capabilities & (1 << 11)):
            raise ValueError("firmware does not advertise Peripheral capability")
        payload = adv_payload.configuration(revision, not args.disabled, args.mode,
                                            args.interval_ms, advertising, response)
        result = probe.command(7, payload)
    print("Applied advertising:", json.dumps(adv_payload.describe_configuration(result)))


def dfu_status(data: bytes) -> dict:
    if len(data) != 62:
        raise ValueError("invalid DFU status length")
    return {"session": struct.unpack_from("<Q", data)[0], "state": data[8],
            "runningConfirmed": bool(data[9]), "acceptedOffset": struct.unpack_from("<I", data, 10)[0],
            "imageSize": struct.unpack_from("<I", data, 14)[0],
            "maxImageSize": struct.unpack_from("<I", data, 18)[0],
            "lastError": struct.unpack_from("<i", data, 22)[0], "sha256": data[26:58].hex(),
            "confirmRemainingMs": struct.unpack_from("<I", data, 58)[0]}


def upgrade_command(probe: Probe, args):
    if not probe.capabilities & (1 << 12):
        raise ValueError("DFU unavailable: install the MCUboot dual-slot build first")
    probe.response_timeout = 30.0  # Flash erase/hash is longer than a normal BLE command.
    if args.dfu_action == "status":
        print(json.dumps(dfu_status(probe.command(0x33))))
    elif args.dfu_action == "confirm":
        if not args.accept_current_image:
            raise ValueError("Confirm only after checking new firmware and health; use --accept-current-image")
        image = Path(args.image).read_bytes()
        probe.command(0x37, struct.pack("<I", len(image)) + hashlib.sha256(image).digest())
        probe.command(0x36)
        print("Running image confirmed")
    elif args.dfu_action in ("abort", "reboot"):
        data = probe.command(0x34 if args.dfu_action == "abort" else 0x35,
                             struct.pack("<Q", args.session))
        print(json.dumps(dfu_status(data)) if data else "Reboot accepted")
    else:
        path = Path(args.image)
        image = path.read_bytes()
        if len(image) < 32 or image[:4] != bytes.fromhex("3db8f396"):
            raise ValueError("Expected an MCUboot signed image, not a HEX file or raw application binary")
        digest = hashlib.sha256(image).digest()
        session = args.session or secrets.randbits(64) or 1
        status = dfu_status(probe.command(0x30, struct.pack("<QI", session, len(image)) + digest))
        if status["sha256"] != digest.hex() or status["imageSize"] != len(image):
            raise ValueError("device download fingerprint does not match this file")
        offset = status["acceptedOffset"]
        print(json.dumps(status))
        while offset < len(image):
            chunk = image[offset:offset + 1024]
            status = dfu_status(probe.command(0x31, struct.pack("<QIH", session, offset, len(chunk)) + chunk))
            if status["acceptedOffset"] != offset + len(chunk):
                raise ValueError("unexpected accepted offset")
            offset = status["acceptedOffset"]
        status = dfu_status(probe.command(0x32, struct.pack("<Q", session)))
        print("Image staged:", json.dumps(status))
        if args.reboot:
            probe.command(0x35, struct.pack("<Q", session))
            print("Trial boot requested; reconnect, inspect firmware/status, then explicitly confirm")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="Serial port, for example COM8")
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--listen", type=float, default=0, help="Seconds to display unacknowledged batches")
    sub = parser.add_subparsers(dest="action")
    sub.add_parser("status")
    scan = sub.add_parser("scan")
    scan.add_argument("state", choices=("on", "off"))
    scan.add_argument("--active", action="store_true")
    adv = sub.add_parser("adv", help="Configure outward Beacon or connectable advertising")
    adv_sub = adv.add_subparsers(dest="adv_action", required=True)
    for action in ("status", "on", "off"):
        adv_sub.add_parser(action)
    for action in ("raw", "ibeacon", "service"):
        spec = adv_sub.add_parser(action)
        spec.add_argument("--mode", choices=tuple(adv_payload.MODES), default="nonconnectable")
        spec.add_argument("--interval-ms", type=int, default=250)
        spec.add_argument("--name")
        spec.add_argument("--disabled", action="store_true", help="Save contents without starting")
        if action == "raw":
            spec.add_argument("--adv-hex", default="020106")
            spec.add_argument("--response-hex", default="")
        else:
            spec.add_argument("--uuid", required=True)
            if action == "ibeacon":
                spec.add_argument("--major", type=int, default=1)
                spec.add_argument("--minor", type=int, default=1)
                spec.add_argument("--measured-power", type=int, default=-59,
                                  help="Calibrated RSSI at 1 m; not PA transmit power")
            else:
                spec.add_argument("--data-hex", default="")
    dfu = sub.add_parser("dfu", help="UART transfer reference for ESP-to-nRF upgrades")
    dfu_sub = dfu.add_subparsers(dest="dfu_action", required=True)
    dfu_sub.add_parser("status")
    confirm = dfu_sub.add_parser("confirm")
    confirm.add_argument("image", help="Expected zephyr.signed.bin, checked against the running slot")
    confirm.add_argument("--accept-current-image", action="store_true")
    for action in ("abort", "reboot"):
        operation = dfu_sub.add_parser(action)
        operation.add_argument("session", type=lambda value: int(value, 0))
    upload = dfu_sub.add_parser("upload")
    upload.add_argument("image", help="zephyr.signed.bin produced by the trusted signing build")
    upload.add_argument("--session", type=lambda value: int(value, 0))
    upload.add_argument("--reboot", action="store_true")
    raw = sub.add_parser("command")
    raw.add_argument("opcode", type=lambda value: int(value, 0))
    raw.add_argument("args_hex", nargs="?", default="")
    args = parser.parse_args()
    if args.self_test:
        assert crc32c(b"123456789") == 0xE3069283
        golden = bytes.fromhex("00 05 47 57 01 01 01 09 08 07 06 05 04 03 02 01 01 01 01 05 4F 42 A1 E8 00")
        assert encode(1, 0x0102030405060708, 0) == golden
        for payload in (b"", bytes(3072), bytes(range(256)) * 12):
            assert decode(encode(0x10, 7, 9, payload)[1:-1]) == (0x10, 7, 9, payload)
        print("UART probe codec: official HELLO vector and maximum frames PASS")
        return
    if not args.port:
        parser.error("--port is required")
    probe = Probe(args.port)
    try:
        probe.hello()
        if args.action == "status":
            data = probe.command(3)
            if len(data) != 71:
                raise ValueError("invalid status length")
            fields = ("uptimeMs", "scanRevision", "scanState", "queuedRecords", "queuedBytes",
                      "reportsSeen", "reportsFiltered", "droppedFull", "droppedIncomplete",
                      "badRxFrames", "batchRetries")
            print(json.dumps(dict(zip(fields, struct.unpack("<QQBHI6Q", data)))))
        elif args.action == "scan":
            profile = bytearray(probe.command(6))
            if len(profile) < 19:
                raise ValueError("short ScanProfile")
            revision = struct.unpack_from("<Q", profile, 2)[0]
            struct.pack_into("<Q", profile, 2, revision + 1)
            profile[10] = (1 if args.state == "on" else 0) | (2 if args.active else 0)
            print("Applied ScanProfile:", probe.command(5, profile).hex())
        elif args.action == "command":
            print("Result:", probe.command(args.opcode, bytes.fromhex(args.args_hex)).hex())
        elif args.action == "adv":
            advertising_command(probe, args)
        elif args.action == "dfu":
            upgrade_command(probe, args)
        deadline = time.monotonic() + args.listen
        while time.monotonic() < deadline:
            frame = probe.next_frame(deadline)
            if frame:
                probe.show_batch(frame)
    finally:
        probe.port.close()


if __name__ == "__main__":
    main()
