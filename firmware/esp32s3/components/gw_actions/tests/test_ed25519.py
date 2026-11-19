"""Cross-implementation Ed25519 test of unmodified production verifier.

Usage: python test_ed25519.py path/to/verify-ed25519.exe
Requires cryptography; test keys are generated in memory and never installed.
"""
import json
import pathlib
import subprocess
import sys
import tempfile
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives import serialization

binary = str(pathlib.Path(sys.argv[1]).resolve())
count = 0
with tempfile.TemporaryDirectory(prefix="gw-ed25519-") as temp:
    sample = pathlib.Path(temp) / "sample.bin"

    def check(key, sig, message, accepted):
        global count
        sample.write_bytes(key + sig + message)
        result = subprocess.run([binary, str(sample)], check=False)
        assert result.returncode == (0 if accepted else 1), (count, result.returncode)
        count += 1

    # RFC 8032 section 7.1 TEST 1: independent fixed empty-message vector.
    check(bytes.fromhex("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a"),
          bytes.fromhex("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555f"
                        "b8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"), b"", True)
    for i in range(24):
        signer = Ed25519PrivateKey.generate()
        public = signer.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
        manifest = {"keyId": "test-release", "name": "GW32 firmware", "kind": "FIRMWARE",
                    "component": "NRF54" if i % 2 else "GATEWAY", "version": "test/" + str(i),
                    "compatibleModels": ["GW32I33V2"], "sha256": "01" * 32}
        raw = json.dumps(manifest, ensure_ascii=False, separators=(",", ":")).encode()
        signature = signer.sign(raw)
        check(public, signature, raw, True)
        check(public, signature, json.dumps(manifest, indent=2).encode(), False)
        check(public, signature, raw + b"\n", False)
        changed = bytearray(signature); changed[i % 64] ^= 1
        check(public, bytes(changed), raw, False)
        different = Ed25519PrivateKey.generate().public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
        check(different, signature, raw, False)
        noncanonical = bytearray(signature); noncanonical[63] |= 0x80
        check(public, bytes(noncanonical), raw, False)
print(f"PASS: {count} RFC/OpenSSL cross-implementation Ed25519 cases")
