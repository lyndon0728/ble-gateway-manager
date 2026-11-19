"""Run production C host tests; SDK is used as the pinned cJSON source provider."""
import argparse
import pathlib
import subprocess
import sys
import tempfile

p = argparse.ArgumentParser()
p.add_argument("--cc", default="cc")
p.add_argument("--idf", required=True)
args = p.parse_args()
subprocess.run([sys.executable, str(pathlib.Path(__file__).with_name("test_package_ota.py"))], check=True)
project = pathlib.Path(__file__).resolve().parents[1]
components = project / "components"
cjson = pathlib.Path(args.idf).resolve() / "components/json/cJSON"
compiler = [args.cc] + (["cc"] if pathlib.Path(args.cc).stem == "zig" else [])
common = ["-std=c11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror"]

with tempfile.TemporaryDirectory(prefix="gw-host-") as temp:
    def build_run(name, sources, includes=(), run=True):
        binary = pathlib.Path(temp) / (name + (".exe" if sys.platform == "win32" else ""))
        command = compiler + common
        for directory in includes:
            command += ["-I", str(directory)]
        command += [str(s) for s in sources] + ["-lm", "-o", str(binary)]
        subprocess.run(command, check=True)
        if run:
            subprocess.run([str(binary)], check=True)
        return binary

    core = components / "gw_core"
    build_run("rules", [core / "tests/test_rules.c", core / "gw_rules.c"], [core])
    build_run("core-state", [core / "tests/test_state.c", core / "gw_rules.c", cjson / "cJSON.c"],
              [core / "include", core / "tests/state_stubs",
               components / "gw_actions/tests/gatt_stubs", cjson])
    provision = components / "gw_provision"
    build_run("guard", [provision / "tests/guard_test.c", provision / "gw_provision_guard.c"], [provision])
    cloud = components / "gw_cloud"
    cloud_includes = [cloud / "tests/mocks"] + [components / name / "include" for name in (
        "gw_cloud", "gw_core", "gw_http", "gw_nrf", "gw_wire", "gw_actions")]
    build_run("cloud", [cloud / "tests/test_cloud.c", cjson / "cJSON.c"], cloud_includes + [cjson])
    actions = components / "gw_actions"
    build_run("gatt", [actions / "tests/test_gatt_nrf.c", cjson / "cJSON.c"],
              [actions / "tests/gatt_stubs", cjson])
    action_command = [sys.executable, str(actions / "tests/run_host.py"),
                      "--cjson-dir", str(cjson), "--compiler", args.cc,
                      "--out", str(pathlib.Path(temp) / "actions")]
    if pathlib.Path(args.cc).stem == "zig":
        action_command += ["--compiler-arg", "cc"]
    subprocess.run(action_command, check=True)
    subprocess.run([sys.executable, str(components / "gw_capture/tests/run_host_tests.py"),
                    "--cc", args.cc, "--idf", args.idf], check=True)
    subprocess.run([sys.executable, str(components / "gw_http/tests/run_host_tests.py"),
                    "--cc", args.cc, "--idf", args.idf], check=True)
print("PASS: production host contracts (device I/O is simulated)")
