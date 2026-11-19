"""Build production state-machine tests with mocked hardware boundaries."""
import argparse
import pathlib
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument("--cjson-dir", required=True, type=pathlib.Path)
parser.add_argument("--compiler", default="cc")
parser.add_argument("--compiler-arg", action="append", default=[])
parser.add_argument("--out", type=pathlib.Path)
args = parser.parse_args()
tests = pathlib.Path(__file__).resolve().parent
component = tests.parent
repo = tests.parents[4]
out = (args.out or repo / ".tools" / "esp-actions-tests").resolve()
out.mkdir(parents=True, exist_ok=True)
cjson = args.cjson_dir.resolve()
assert (cjson / "cJSON.c").is_file() and (cjson / "cJSON.h").is_file()
vendor = component / "vendor" / "monocypher"
crypto = [vendor / "monocypher.c", vendor / "monocypher-ed25519.c"]
include_dirs = [tests / "action_stubs", component.parent / "gw_nrf" / "include",
                component.parent / "gw_http" / "include", tests / "gatt_stubs",
                component / "include", component.parent / "gw_wire" / "include", vendor, cjson]
base = [args.compiler, *args.compiler_arg, "-std=c11", "-Wall", "-Wextra", "-Werror"]
base += ["-I" + str(path) for path in include_dirs]
for name in ("actions", "ota"):
    binary = out / (name + (".exe" if sys.platform == "win32" else ""))
    source = [tests / f"test_{name}.c", repo / "firmware" / "nrf54l15" / "src" / "gw_core.c", cjson / "cJSON.c", *crypto]
    subprocess.run([*base, *map(str, source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
binary = out / ("verify-ed25519.exe" if sys.platform == "win32" else "verify-ed25519")
subprocess.run([*base, str(tests / "verify_ed25519.c"), *map(str, crypto), "-o", str(binary)], check=True)
subprocess.run([sys.executable, str(tests / "test_ed25519.py"), str(binary)], check=True)
