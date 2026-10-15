"""Embed only hash-verified recorded fixtures and independent native goldens."""
from pathlib import Path
import hashlib
import json
import sys
root = Path(__file__).resolve().parents[1] / "tests/fixtures"
manifest = json.loads((root / "manifest.json").read_text())
def fp(value):
    text = format(value, ".9g")
    return text + ("f" if "." in text or "e" in text else ".0f")
lines = ['#pragma once', '#include "xt4_face.h"',
         'struct Fixture {const char*name;const uint8_t*image;uint32_t count;const xt4_face_box*expected;};']
names = []
for row in manifest["fixtures"]:
    name = row["name"]
    symbol = name.replace("-", "_")
    raw = (root / (name + ".bgr")).read_bytes()
    ref = (root / (name + ".reference.json")).read_bytes()
    if hashlib.sha256(raw).hexdigest() != row["sha256"] or hashlib.sha256(ref).hexdigest() != row["reference_sha256"]:
        raise SystemExit("Fixture integrity mismatch: " + name)
    boxes = json.loads(ref)["boxes"]
    lines.append("static const uint8_t image_" + symbol + "[76800]={" + ",".join(map(str, raw)) + "};")
    encoded = []
    for b in boxes:
        encoded.append("{{" + ",".join(fp(b[k]) for k in ("x0", "y0", "x1", "y1")) + "}," + fp(b["score"]) + "}")
    lines.append("static const xt4_face_box expected_" + symbol + "[" + str(max(1, len(boxes))) + "]={" + (",".join(encoded) if encoded else "{}") + "};")
    names.append('{"' + name + '",image_' + symbol + "," + str(len(boxes)) + ",expected_" + symbol + "}")
lines.append("static const Fixture fixtures[]={" + ",".join(names) + "};")
Path(sys.argv[1]).write_text("\n".join(lines) + "\n")
