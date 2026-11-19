"""Compile production Peripheral orchestration with explicit crypto/I/O mocks."""
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
with tempfile.TemporaryDirectory(prefix="gw-peripheral-host-") as tmp:
    binary=Path(tmp)/("test.exe" if sys.platform=="win32" else "test")
    command=compiler+["-std=c11","-Wall","-Wextra","-Werror","-D_GNU_SOURCE"]
    for include in (here/"mocks",here.parent/"include",cjson):
        command += ["-I",str(include)]
    command += [str(here/"test_peripheral.c"),str(here.parent/"gw_pg.c"),str(cjson/"cJSON.c"),"-lm","-o",str(binary)]
    subprocess.run(command,check=True)
    subprocess.run([str(binary)],check=True)
