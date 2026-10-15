"""Explicit offline fixture generation; never run from ordinary tests."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import cv2 as cv
import numpy as np

p = argparse.ArgumentParser()
p.add_argument("--source", type=Path, required=True)
p.add_argument("--reference", type=Path, required=True)
p.add_argument("--output", type=Path, default=Path("tests/fixtures"))
a = p.parse_args()
source_hash = hashlib.sha256(a.source.read_bytes()).hexdigest()
if source_hash != "88431cd9653ccd539741b555fb0a46b61558b301d4110412b5bc28b5e3ea6cb5":
    raise SystemExit("Unexpected astronaut asset")
im = cv.imread(str(a.source))
if im is None:
    raise SystemExit("Cannot decode source")
small = cv.resize(im, (160, 160), interpolation=cv.INTER_LINEAR)
translated = cv.warpAffine(small, np.float32([[1, 0, 15], [0, 1, 8]]), (160, 160))
dark = np.floor(small.astype(np.float32) * 0.6 + 0.5).astype(np.uint8)
crop = cv.resize(im[40:190, 165:290], (80, 80), interpolation=cv.INTER_LINEAR)
crowd = np.tile(crop, (2, 2, 1))
non_square = np.zeros((160, 160, 3), np.uint8)
non_square[33:127] = cv.resize(im[:300], (160, 94))
fixtures = {"astronaut": small, "blank": np.zeros_like(small), "translated": translated,
            "darkened": dark, "non-square": non_square, "crowd": crowd}
a.output.mkdir(parents=True, exist_ok=True)
rows = []
for name, image in fixtures.items():
    raw = a.output / (name + ".bgr")
    raw.write_bytes(image.tobytes())
    cv.imwrite(str(a.output / (name + ".png")), image)
    result_path = a.output / (name + ".reference.json")
    subprocess.run([str(a.reference.resolve()), "--input", str(raw), "--output", str(result_path)], check=True)
    result = json.loads(result_path.read_text())
    display = image.copy()
    for box in result["boxes"]:
        cv.rectangle(display, (round(box["x0"]), round(box["y0"])), (round(box["x1"]), round(box["y1"])), (0,255,0), 1)
    cv.imwrite(str(a.output / (name + ".reference.png")), display)
    rows.append({"name": name, "width": 160, "height": 160, "stride": 480, "bytes": 76800,
                 "sha256": hashlib.sha256(raw.read_bytes()).hexdigest(), "reference_boxes": len(result["boxes"]),
                 "reference_sha256": hashlib.sha256(result_path.read_bytes()).hexdigest()})
manifest = {"source_sha256": source_hash, "source_dimensions": [512,512],
 "generation": {"resize": "OpenCV INTER_LINEAR", "translation": [15,8], "darkening": 0.6,
                "crop_xywh": [165,40,125,150], "crowd": "2x2 80-pixel crops",
                "non_square": "top 300 source rows resized to 160x94, y=33"},
 "tools": {"opencv": cv.__version__, "numpy": np.__version__}, "fixtures": rows}
(a.output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
print([(r["name"], r["reference_boxes"]) for r in rows])
