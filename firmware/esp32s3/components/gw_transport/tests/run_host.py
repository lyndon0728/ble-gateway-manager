"""Compile actual transport adapter against an explicit MQTT event-loop mock."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile
p=argparse.ArgumentParser()
p.add_argument("--cc",default="cc")
p.add_argument("--idf",required=True)
args=p.parse_args()
here=Path(__file__).resolve().parent
cjson=Path(args.idf).resolve()/"components/json/cJSON"
compiler=[args.cc]+(["cc"] if Path(args.cc).stem=="zig" else [])
with tempfile.TemporaryDirectory(prefix="gw-transport-adapter-") as tmp:
    binary=Path(tmp)/("test.exe" if sys.platform=="win32" else "test")
    command=compiler+["-std=c11","-Wall","-Wextra","-Werror","-D_GNU_SOURCE"]
    for include in (here/"mocks",here.parent/"include",here.parent,cjson):command += ["-I",str(include)]
    command += [str(here/"test_adapter.c"),str(here.parent/"gw_transport_rules.c"),str(cjson/"cJSON.c"),"-lm","-o",str(binary)]
    subprocess.run(command,check=True)
    subprocess.run([str(binary)],check=True)
