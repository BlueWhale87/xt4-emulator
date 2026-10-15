import json
from pathlib import Path
import re
import subprocess
import sys
binary, output = sys.argv[1:]
text = subprocess.check_output(["/usr/bin/size", "-m", binary], text=True)
segment = ""
rows = []
for line in text.splitlines():
    m = re.match(r"Segment (\S+):", line)
    if m:
        segment = m[1]
    m = re.search(r"Section (\S+): (\d+)", line)
    if m:
        rows.append({"segment": segment, "section": m[1], "bytes": int(m[2])})
code = sum(r["bytes"] for r in rows if r["section"] in ("__text", "__stubs", "__stub_helper", "__auth_stubs"))
rodata = sum(r["bytes"] for r in rows if r["section"] in ("__const", "__cstring", "__literal4", "__literal8", "__literal16"))
data = sum(r["bytes"] for r in rows if r["segment"].startswith("__DATA") and r["section"] != "__const")
Path(output).write_text(json.dumps({"scope": "whole host runner, not isolated core", "code_bytes": code,
 "rodata_bytes": rodata, "static_data_bytes": data, "sections": rows,
 "command": ["/usr/bin/size", "-m", binary]}, separators=(",", ":")) + "\n")
