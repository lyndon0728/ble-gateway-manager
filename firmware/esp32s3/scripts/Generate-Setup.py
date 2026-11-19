"""Create device-local Security 2 verifier material; never print/store the password.

This does not flash a device, create a shared default password, burn eFuses, or
replace the separately approved encrypted-NVS manufacturing workflow.
"""
import argparse
import getpass
import hashlib
import json
import os
from pathlib import Path
import secrets
import sys

PROJECT = Path(__file__).resolve().parent.parent
REFERENCE = PROJECT / "components/gw_provision/tests/reference"
sys.path.insert(0, str(REFERENCE))
from srp6a import get_ng, NG_3072  # pinned official public group constants only

N, G = get_ng(NG_3072)


def verifier(username: str, password: str, salt: bytes) -> bytes:
    if not 1 <= len(username.encode("utf-8")) <= 64 or len(salt) != 16:
        raise ValueError("username must contain 1-64 UTF-8 bytes; salt is 16 bytes")
    identity_hash = hashlib.sha512((username + ":" + password).encode("utf-8")).digest()
    # Preserve all digest/salt bytes, including leading zeros, as esp_srp.c does.
    x = int.from_bytes(hashlib.sha512(salt + identity_hash).digest(), "big")
    return pow(G, x, N).to_bytes(384, "big")


def self_test() -> None:
    tests = PROJECT / "components/gw_provision/tests"
    count = 0
    for name in ["security2-vectors.json", "security2-device-edge-vectors.json"]:
        for item in json.loads((tests / name).read_text(encoding="utf-8")):
            v = int.from_bytes(verifier(item["username"], item["password"], bytes.fromhex(item["salt"])), "big")
            index = item["username"].rsplit("-", 1)[1]
            secret = int.from_bytes(hashlib.sha256(("server-" + index).encode()).digest(), "big")
            k = int.from_bytes(hashlib.sha512(N.to_bytes(384, "big") + G.to_bytes(384, "big")).digest(), "big")
            public = (k * v + pow(G, secret, N)) % N
            assert public == int(item["serverPublic"], 16), item["username"]
            count += 1
    print(f"PASS: {count} independent Security 2 server-public vectors, including leading zeros")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--username")
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return
    if not args.username or args.output_dir is None:
        parser.error("--username and --output-dir are required")
    destination = args.output_dir.resolve()
    repository = PROJECT.parent.parent
    if destination.is_relative_to(repository):
        parser.error("Manufacturing material must be stored outside the source checkout")
    password = getpass.getpass("Device-specific setup password (at least 16 characters): ")
    if len(password) < 16 or password != getpass.getpass("Repeat password: "):
        parser.error("Passwords must match and contain at least 16 characters")
    salt = secrets.token_bytes(16)
    encoded = verifier(args.username, password, salt)
    password = None
    destination.mkdir(mode=0o700, parents=True, exist_ok=True)
    for name, data in [("sec2_salt.bin", salt), ("sec2_verifier.bin", encoded)]:
        path = destination / name
        descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(data)
    print("Created salt/verifier files. Import them into encrypted factory NVS using the manufacturing workflow.")


if __name__ == "__main__":
    main()
