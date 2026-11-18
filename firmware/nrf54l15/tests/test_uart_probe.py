import contextlib
import hashlib
import io
from pathlib import Path
import struct
import sys
import tempfile
from types import SimpleNamespace
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import uart_probe


class FakeUpgradePeer:
    capabilities = 1 << 12

    def __init__(self):
        self.calls = []
        self.received = bytearray()
        self.state = 0
        self.session = 0
        self.size = 0
        self.digest = bytes(32)

    def command(self, opcode, args=b""):
        self.calls.append((opcode, bytes(args)))
        if opcode == 0x30:
            assert len(args) == 44
            self.session, self.size = struct.unpack_from("<QI", args)
            self.digest = args[12:]
            self.state = 1
        elif opcode == 0x31:
            session, offset, size = struct.unpack_from("<QIH", args)
            assert session == self.session and offset == len(self.received)
            assert size == len(args) - 14 and 0 < size <= 1024
            self.received.extend(args[14:])
        elif opcode == 0x32:
            assert struct.unpack("<Q", args)[0] == self.session
            assert hashlib.sha256(self.received).digest() == self.digest
            self.state = 2
        else:
            return b""
        return (struct.pack("<QBBIIIi", self.session, self.state, 1, len(self.received),
                            self.size, 600 * 1024, 0) + self.digest + bytes(4))


class UpgradeProbeTests(unittest.TestCase):
    def test_exact_chunk_layout_and_no_automatic_confirmation(self):
        with tempfile.TemporaryDirectory() as directory:
            image = bytes.fromhex("3db8f396") + bytes(1025)
            path = Path(directory) / "test.signed.bin"
            path.write_bytes(image)
            peer = FakeUpgradePeer()
            with contextlib.redirect_stdout(io.StringIO()):
                uart_probe.upgrade_command(peer, SimpleNamespace(dfu_action="upload", image=path,
                                                                session=42, reboot=True))
            self.assertEqual(peer.received, image)
            self.assertEqual([op for op, _ in peer.calls], [0x30, 0x31, 0x31, 0x32, 0x35])
            self.assertEqual(peer.response_timeout, 30.0)

    def test_confirm_verifies_expected_running_bytes_first(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "expected.signed.bin"
            image = b"expected signed artifact bytes"
            path.write_bytes(image)
            peer = FakeUpgradePeer()
            with contextlib.redirect_stdout(io.StringIO()):
                uart_probe.upgrade_command(peer, SimpleNamespace(dfu_action="confirm", image=path,
                                                                accept_current_image=True))
            self.assertEqual([op for op, _ in peer.calls], [0x37, 0x36])
            self.assertEqual(peer.calls[0][1], struct.pack("<I", len(image)) + hashlib.sha256(image).digest())
            self.assertEqual(peer.response_timeout, 30.0)

    def test_unsupported_and_invalid_container_do_not_write(self):
        peer = FakeUpgradePeer()
        peer.capabilities = 0
        with self.assertRaises(ValueError):
            uart_probe.upgrade_command(peer, SimpleNamespace(dfu_action="status"))
        self.assertEqual(peer.calls, [])
        peer.capabilities = 1 << 12
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "firmware.hex"
            path.write_text(":04000000DEADBEEF00")
            with self.assertRaises(ValueError):
                uart_probe.upgrade_command(peer, SimpleNamespace(dfu_action="upload", image=path,
                                                                session=None, reboot=False))
        self.assertEqual(peer.calls, [])


if __name__ == "__main__":
    unittest.main()
