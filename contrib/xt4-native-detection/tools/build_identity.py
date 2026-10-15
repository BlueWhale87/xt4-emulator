from pathlib import Path
import hashlib
import json
import subprocess
import sys
root = Path(__file__).resolve().parents[1]
files = sorted([*root.joinpath("src").glob("*.*"), *root.joinpath("include").glob("*.h")])
rows = {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in files}
encoded = json.dumps(rows, sort_keys=True, separators=(",", ":")).encode()
metadata = {"native_source_sha256": hashlib.sha256(encoded).hexdigest(), "files": rows,
            "compiler": sys.argv[2], "compiler_version": sys.argv[3], "build_type": sys.argv[4],
            "git_commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip(),
            "git_dirty": bool(subprocess.check_output(["git", "status", "--porcelain"], cwd=root, text=True).strip()),
            "scalar": True, "fast_math": False, "fp_contract": "off"}
Path(sys.argv[1]).write_text("#define XT4_BUILD_METADATA " + json.dumps(json.dumps(metadata, separators=(",", ":"))) + "\n")
