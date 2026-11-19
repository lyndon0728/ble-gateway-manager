"""Exercise production HTTP framing and HTTPS adapter with SDK I/O replacements."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc",default="cc")
    parser.add_argument("--idf",default=os.environ.get("IDF_PATH"),required=not os.environ.get("IDF_PATH"))
    parser.add_argument("--out")
    args=parser.parse_args()
    component=Path(__file__).resolve().parents[1]
    cc=[args.cc]+(["cc"] if Path(args.cc).stem=="zig" else [])
    common=["-std=c11","-Wall","-Wextra","-Werror",f"-I{component / 'include'}"]
    if os.name!="nt":common.append("-D_POSIX_C_SOURCE=200809L")
    with tempfile.TemporaryDirectory(prefix="gw_http_") as tmp:
        out=Path(args.out).resolve() if args.out else Path(tmp)
        out.mkdir(parents=True,exist_ok=True)
        suffix=".exe" if os.name=="nt" else ""
        framing=out/("test_parser"+suffix)
        subprocess.run([*cc,*common,str(component/"tests/test_parser.c"),str(component/"gw_http_parser.c"),"-o",str(framing)],check=True)
        subprocess.run([str(framing)],check=True,timeout=60)
        json_dir=Path(args.idf).resolve()/"components/json/cJSON"
        adapter=out/("test_http"+suffix)
        subprocess.run([*cc,*common,f"-I{component/'tests/mocks'}",f"-I{json_dir}",str(component/"tests/test_http.c"),str(component/"gw_http_parser.c"),str(json_dir/"cJSON.c"),"-o",str(adapter)],check=True)
        subprocess.run([str(adapter)],check=True,timeout=60)

if __name__=="__main__":main()
