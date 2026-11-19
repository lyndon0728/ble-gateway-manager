"""Create one signed package for the local OTA page and platform artifact upload.

Requires cryptography. Use an external Ed25519 PEM release key; never put release
private keys in the repository. The device remains responsible for full ESP image
validation. This tool checks the target/header and binds exact bytes to the signed
manifest; it does not enable Secure Boot or modify device eFuses.
"""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import re
import struct

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

OTA_PARTITION_SIZE = 0x280000


def app_version(image):
    """Read IDF image header + first segment's esp_app_desc_t (IDF 5.5.6)."""
    if not 288 <= len(image) <= OTA_PARTITION_SIZE:
        raise ValueError("image must contain an app descriptor and fit the 0x280000 OTA partition")
    if image[0] != 0xE9 or not 1 <= image[1] <= 16:
        raise ValueError("expected an ESP-IDF application .bin, not a merged flash image")
    if struct.unpack_from("<H", image, 12)[0] != 9:
        raise ValueError("image chip ID is not ESP32-S3")
    first_size = struct.unpack_from("<I", image, 28)[0]
    if first_size < 256 or 32 + first_size > len(image):
        raise ValueError("invalid first image segment")
    if struct.unpack_from("<I", image, 32)[0] != 0xABCD5432:
        raise ValueError("missing ESP application descriptor")
    version_bytes = image[48:80]
    if b"\0" not in version_bytes:
        raise ValueError("application version is not NUL-terminated")
    version = version_bytes.split(b"\0", 1)[0].decode("utf-8", errors="strict")
    if not version or any(ord(c) < 32 for c in version):
        raise ValueError("invalid application version")
    return version


def identifiers(values, label, required=False):
    values = list(dict.fromkeys(values or []))
    if required and not values:
        raise ValueError(f"at least one {label} is required")
    if len(values) > 128 or any(not value or len(value.encode("utf-8")) > 128 or
                                any(ord(c) < 32 for c in value) for value in values):
        raise ValueError(f"{label} values must be nonempty strings up to 128 UTF-8 bytes")
    return values


def package(image_path, key_path, key_id, models, output_dir, *, name="gw32_esp32s3",
            hardware=None, from_versions=None, password=None):
    if not re.fullmatch(r"[A-Za-z0-9_-]{1,128}", key_id):
        raise ValueError("key ID must use 1..128 letters, digits, underscores or hyphens")
    identifiers([name], "name", True)
    image = Path(image_path).read_bytes()
    version = app_version(image)
    manifest = {
        "kind": "FIRMWARE", "component": "GATEWAY", "imageFormat": "ESP_IDF_APP_BIN",
        "name": name, "version": version, "sizeBytes": len(image),
        "sha256": hashlib.sha256(image).hexdigest(), "keyId": key_id,
        "compatibleModels": identifiers(models, "model", True),
    }
    for field, values in (("hardwareVersions", hardware), ("fromFirmwareVersions", from_versions)):
        checked = identifiers(values, field)
        if checked:
            manifest[field] = checked
    raw = json.dumps(manifest, separators=(",", ":"), ensure_ascii=True)
    if len(raw.encode("utf-8")) > 8192:
        raise ValueError("signed manifest exceeds the local OTA 8192-byte limit")
    key = serialization.load_pem_private_key(Path(key_path).read_bytes(), password=password)
    if not isinstance(key, Ed25519PrivateKey):
        raise ValueError("release private key must be Ed25519 PEM")
    signature = base64.b64encode(key.sign(raw.encode("utf-8"))).decode("ascii")
    # Test the complete encrypted endpoint's plaintext envelope limit as well:
    # escaping the manifest can make it larger than its signed raw byte count.
    envelope = {"apiVersion": "1.0", "requestId": "r" * 64, "operation": "OTA_BEGIN",
                "expectedNetworkGeneration": "9" * 20, "baseLocalRevision": "9" * 20,
                "body": {"uploadId": "u" * 64, "manifest": raw, "signature": signature}}
    if len(json.dumps(envelope, separators=(",", ":")).encode("utf-8")) > 16384:
        raise ValueError("manifest exceeds the encrypted local request limit after escaping")
    output = Path(output_dir)
    output.mkdir(parents=True, exist_ok=False)
    files = {"firmware.bin": image, "manifest.json": raw.encode("utf-8"),
             "signature.txt": signature.encode("ascii"),
             "verification.json": json.dumps({"manifest": raw, "signature": signature},
                                             ensure_ascii=True, indent=2).encode("utf-8")}
    for filename, content in files.items():
        with (output / filename).open("xb") as stream:
            stream.write(content)
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--private-key", type=Path, required=True)
    parser.add_argument("--key-id", required=True)
    parser.add_argument("--model", action="append", required=True)
    parser.add_argument("--hardware", action="append")
    parser.add_argument("--from-version", action="append")
    parser.add_argument("--name", default="gw32_esp32s3")
    parser.add_argument("--output-dir", type=Path, required=True, help="new output directory; never overwritten")
    parser.add_argument("--password-env", help="environment variable containing the encrypted PEM password")
    args = parser.parse_args()
    try:
        password = None
        if args.password_env:
            value = os.environ.get(args.password_env)
            if not value:
                raise ValueError("private-key password environment variable is missing or empty")
            password = value.encode("utf-8")
        manifest = package(args.image, args.private_key, args.key_id, args.model, args.output_dir,
                           name=args.name, hardware=args.hardware, from_versions=args.from_version,
                           password=password)
    except (OSError, ValueError, TypeError) as error:
        parser.exit(2, f"Package rejected: {error}\n")
    print(f"Created {args.output_dir}: ESP32-S3 {manifest['version']}, {manifest['sizeBytes']} bytes")
    print(f"SHA-256: {manifest['sha256']}")
    print("Local page: select verification.json and firmware.bin. Keep the private key offline.")


if __name__ == "__main__":
    main()
