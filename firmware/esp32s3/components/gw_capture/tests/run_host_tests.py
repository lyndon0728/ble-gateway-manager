"""Build the production capture sources against host I/O and validate real JSON.

Requires a C11 compiler (or Zig), ESP-IDF 5.5.x source dependencies and jsonschema.
No application firmware or live device is contacted by these tests.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="cc", help="C compiler, or path to zig")
    parser.add_argument("--idf", default=os.environ.get("IDF_PATH"), required=not os.environ.get("IDF_PATH"))
    parser.add_argument("--out", help="Keep binaries/JSON in this directory")
    args = parser.parse_args()
    import jsonschema

    repo = Path(__file__).resolve().parents[5]
    component = Path(__file__).resolve().parents[1]
    idf = Path(args.idf).resolve()
    cc = [args.cc] + (["cc"] if Path(args.cc).stem == "zig" else [])
    common = ["-std=c11", "-Wall", "-Wextra", "-Werror", f"-I{component / 'include'}",
              f"-I{component.parent / 'gw_wire/include'}", f"-I{component.parent / 'gw_transport/include'}"]
    if os.name != "nt":
        common.append("-D_POSIX_C_SOURCE=200809L")
    sources = [component / "gw_spool.c", component / "gw_capture_rules.c", repo / "firmware/nrf54l15/src/gw_core.c"]
    with tempfile.TemporaryDirectory(prefix="gw_capture_") as temporary:
        out = Path(args.out).resolve() if args.out else Path(temporary)
        out.mkdir(parents=True, exist_ok=True)
        suffix = ".exe" if os.name == "nt" else ""
        spool_exe = out / ("test_spool" + suffix)
        subprocess.run([*cc, *common, str(component / "tests/test_spool.c"), *map(str, sources), "-o", str(spool_exe)], check=True)
        subprocess.run([str(spool_exe)], check=True, timeout=120)
        mbedtls = idf / "components/mbedtls/mbedtls"
        json_dir = idf / "components/json/cJSON"
        capture_exe = out / ("test_capture" + suffix)
        flags = [f"-I{component / 'tests/mocks'}", f"-I{json_dir}", f"-I{mbedtls / 'include'}",
                 "-DCONFIG_GW_REQUIRE_FLASH_ENCRYPTION=1", '-DMBEDTLS_CONFIG_FILE="test_mbedtls_config.h"']
        crypto = [mbedtls / "library" / f"{name}.c" for name in ("sha256", "base64", "platform_util", "platform", "constant_time")]
        subprocess.run([*cc, *common, *flags, str(component / "tests/test_capture.c"), *map(str, sources), str(json_dir / "cJSON.c"), *map(str, crypto), "-lm", "-o", str(capture_exe)], check=True)
        generated = out / "events.json"
        subprocess.run([str(capture_exe), str(generated)], check=True, timeout=120)
        schema = json.loads((repo / "server/internal/platform/assets/message-contracts.schema.json").read_text(encoding="utf-8"))
        schema["$ref"] = "#/$defs/IngressBatch"
        validator = jsonschema.Draft202012Validator(schema, format_checker=jsonschema.FormatChecker())
        batches = json.loads(generated.read_text(encoding="utf-8"))
        for batch in batches:
            validator.validate(batch)
        print(f"capture: {len(batches)} production JSON batches validated against platform IngressBatch 1.1")


if __name__ == "__main__":
    main()
