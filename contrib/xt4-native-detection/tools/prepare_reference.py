"""Verify pristine sources, then materialize the independently patched host reference."""
from pathlib import Path
import difflib
import hashlib
import json
import sys

root = Path(__file__).resolve().parents[1]
out = Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)
lock = json.loads((root / "vendor/source-lock.json").read_text())
patch = []
trace_patch = []
for item in lock["files"]:
    p = root / item["path"]
    data = p.read_bytes()
    if hashlib.sha256(data).hexdigest() != item["sha256"]:
        raise SystemExit("Source hash mismatch: " + item["path"])
    if p.suffix not in (".cpp", ".h"):
        continue
    original = data.decode()
    modified = original
    if p.name == "facedetectcnn.h":
        modified = modified.replace("    int x;","    float x;").replace("    int y;","    float y;")
        modified = modified.replace("    int w;","    float w;").replace("    int h;","    float h;")
        modified = modified.replace("objectdetect_cnn(const unsigned char*", "objectdetect_cnn(unsigned char*")
    if p.name == "facedetectcnn-model.cpp":
        modified = modified.replace("0.45f, 0.2f, 1000, 512","0.3f, 0.8f, 1000, 16")
    if p.name == "facedetectcnn.cpp":
        for expr in ["pp.second.xmin", "pp.second.ymin", "pp.second.xmax - pp.second.xmin", "pp.second.ymax - pp.second.ymin"]:
            modified = modified.replace("int(" + expr + ")", "(" + expr + ")")
    patch.extend(difflib.unified_diff(original.splitlines(True), modified.splitlines(True),
                  fromfile="a/" + p.name, tofile="b/" + p.name))
    traced = modified
    if p.name == "facedetectcnn-model.cpp":
        traced = traced.replace('#include "facedetectcnn.h"', '#include "facedetectcnn.h"\nextern void xt4_ref_trace(const char*, const CDataBlob<float>&);')
        for statement, label, tensor in [
            ("fx = convolution(fx, g_pFilters[0]);", "early", "fx"),
            ("auto fb1 = convolution4layerUnit(fx, g_pFilters[11], g_pFilters[12], g_pFilters[13], g_pFilters[14]);", "middle", "fb1"),
            ("sigmoid(obj);", "cls", "cls")]:
            traced = traced.replace(statement, statement + '\n    xt4_ref_trace("' + label + '", ' + tensor + ');')
        traced = traced.replace('xt4_ref_trace("cls", cls);', 'xt4_ref_trace("cls", cls);\n    xt4_ref_trace("reg", reg);\n    xt4_ref_trace("kps", kps);\n    xt4_ref_trace("obj", obj);')
    trace_patch.extend(difflib.unified_diff(modified.splitlines(True), traced.splitlines(True),
                       fromfile="float/" + p.name, tofile="traced/" + p.name))
    (out / p.name).write_text(traced)
patch_text = "".join(patch)
(root / "reference").mkdir(exist_ok=True)
(root / "reference/float-reference.patch").write_text(patch_text)
(root / "reference/layer_trace.patch").write_text("".join(trace_patch))
(out / "facedetection_export.h").write_text("#pragma once\n#define FACEDETECTION_EXPORT\n")
source_hash = hashlib.sha256((root / "vendor/source-lock.json").read_bytes()).hexdigest()
model_hash = next(x["sha256"] for x in lock["files"] if x["path"].endswith("-data.cpp"))
(out / "reference_identity.h").write_text(
    '#define XT4_REF_SOURCE "' + source_hash + '"\n#define XT4_REF_MODEL "' + model_hash +
    '"\n#define XT4_REF_PATCH "' + hashlib.sha256(patch_text.encode()).hexdigest() + '"\n')
