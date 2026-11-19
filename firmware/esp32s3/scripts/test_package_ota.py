"""Test package trust and exact-byte compatibility without device hardware."""
import base64
import hashlib
import json
from pathlib import Path
import struct
import tempfile
import unittest

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives.asymmetric import ec

from package_ota import OTA_PARTITION_SIZE, app_version, package


class PackageTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.image = bytearray(512)
        self.image[0:2] = bytes([0xE9, 1])
        struct.pack_into("<H", self.image, 12, 9)
        struct.pack_into("<I", self.image, 28, 256)
        struct.pack_into("<I", self.image, 32, 0xABCD5432)
        self.image[48:53] = b"0.2.0"
        self.image_path = self.root / "app.bin"
        self.image_path.write_bytes(self.image)
        self.key = Ed25519PrivateKey.generate()  # Per-test disposable key, never a release key.
        self.key_path = self.root / "key.pem"
        self.key_path.write_bytes(self.key.private_bytes(serialization.Encoding.PEM,
                                  serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
        self.out = self.root / "package"

    def create(self, **kw):
        return package(self.image_path, self.key_path, "release_1", ["GW32I33V2"], self.out, **kw)

    def test_exact_bytes_signatures_and_payload_binding(self):
        result = self.create(hardware=["R2"], from_versions=["0.1.0"])
        envelope = json.loads((self.out / "verification.json").read_text())
        raw = (self.out / "manifest.json").read_bytes()
        self.assertEqual(envelope["manifest"].encode(), raw)
        self.assertEqual(json.loads(raw), result)
        signature = base64.b64decode(envelope["signature"], validate=True)
        self.key.public_key().verify(signature, raw)
        with self.assertRaises(InvalidSignature):
            self.key.public_key().verify(signature, raw + b"\n")
        payload = (self.out / "firmware.bin").read_bytes()
        self.assertEqual(hashlib.sha256(payload).hexdigest(), result["sha256"])
        self.assertEqual(result["sizeBytes"], len(payload))
        self.assertEqual(result["version"], "0.2.0")
        self.assertEqual(result["component"], "GATEWAY")
        self.assertEqual(len(list(self.out.iterdir())), 4)

    def test_refuses_wrong_chip_bootloader_truncation_and_oversize(self):
        for offset, value in ((0, 0), (12, 0), (32, 0)):
            damaged = bytearray(self.image)
            damaged[offset] = value
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                app_version(damaged)
        for payload in (self.image[:200], self.image + bytes(OTA_PARTITION_SIZE)):
            with self.assertRaises(ValueError):
                app_version(payload)

    def test_nonterminated_version_and_invalid_segment(self):
        self.image[48:80] = b"x" * 32
        with self.assertRaises(ValueError):
            app_version(self.image)
        struct.pack_into("<I", self.image, 28, 0xFFFFFFFF)
        with self.assertRaises(ValueError):
            app_version(self.image)

    def test_refuses_overwrite(self):
        self.create()
        existing = (self.out / "firmware.bin").read_bytes()
        with self.assertRaises(FileExistsError):
            self.create()
        self.assertEqual((self.out / "firmware.bin").read_bytes(), existing)

    def test_requires_ed25519_and_compatibility(self):
        wrong = ec.generate_private_key(ec.SECP256R1())
        self.key_path.write_bytes(wrong.private_bytes(serialization.Encoding.PEM,
                                  serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
        with self.assertRaisesRegex(ValueError, "Ed25519"):
            self.create()
        self.assertFalse(self.out.exists())
        with self.assertRaises(ValueError):
            package(self.image_path, self.key_path, "release_1", [], self.out)

    def test_encrypted_pem_and_manifest_limit(self):
        self.key_path.write_bytes(self.key.private_bytes(serialization.Encoding.PEM,
                                  serialization.PrivateFormat.PKCS8,
                                  serialization.BestAvailableEncryption(b"temporary test password")))
        with self.assertRaises(TypeError):
            self.create()
        self.create(password=b"temporary test password")
        with self.assertRaisesRegex(ValueError, "8192"):
            package(self.image_path, self.key_path, "release_1",
                    [str(n) + "x" * 120 for n in range(128)], self.root / "too_large")


if __name__ == "__main__":
    unittest.main()
