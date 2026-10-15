"""Audit the standalone ARM diagnostic; never execute firmware."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess

class ArmAuditError(ValueError):
    pass

def validate_arm_attributes(attrs):
    if attrs.get("Class") != "ELF32" or attrs.get("Machine") != "ARM" or "little endian" not in attrs.get("Data", ""):
        raise ArmAuditError("Expected little-endian ELF32 ARM")
    if attrs.get("Tag_CPU_arch") != "v7" or attrs.get("Tag_CPU_arch_profile", "Application") != "Application":
        raise ArmAuditError("Expected ARMv7-A profile")
    if "VFP registers" in attrs.get("Tag_ABI_VFP_args", "") or "hard-float" in attrs.get("Flags", ""):
        raise ArmAuditError("Hard-float ABI rejected")
    if attrs.get("Tag_Advanced_SIMD_arch") or attrs.get("Tag_FP_arch"):
        raise ArmAuditError("Hardware FP/SIMD requirements rejected for soft profile")

def validate_instructions(mnemonics):
    rejected = [x for x in mnemonics if x.lower().startswith("v")]
    if rejected:
        raise ArmAuditError("FP/SIMD instructions: " + ", ".join(sorted(set(rejected))))

def instruction_mnemonics(disassembly):
    return re.findall(r"^\s*[0-9a-f]+:\s+(?:(?:[0-9a-f]{8}|[0-9a-f]{4})\s+)+\s*([a-z][a-z0-9.]*)", disassembly, re.M)

def validate_entry(entry, segments):
    if entry % 4 or not any(s["executable"] and s["start"] <= entry < s["start"] + s.get("file_bytes", s["bytes"]) for s in segments):
        raise ArmAuditError("Entry outside file-backed executable memory")

def validate_symbols(undefined, defined):
    if undefined:
        raise ArmAuditError("Unresolved symbols: " + ", ".join(undefined))
    forbidden = {"malloc", "calloc", "realloc", "free", "exit", "_exit", "abort", "_sbrk",
                 "_write", "_read", "fopen", "open", "pthread_create"}
    rejected = [x for x in defined if x in forbidden or x.startswith(("_Zn", "_Zd")) or
                (x.startswith("__atomic_") and x.endswith("_8"))]
    if rejected:
        raise ArmAuditError("Forbidden runtime dependencies: " + ", ".join(rejected))

def read_elf(path):
    raw = Path(path).read_bytes()
    if len(raw) < 52 or raw[:6] != b"\x7fELF\x01\x01":
        raise ArmAuditError("Expected ELF32 little endian")
    fields = struct.unpack_from("<HHIIIIIHHHHHH", raw, 16)
    kind, machine, version, entry, phoff = fields[:5]
    phentsize, phnum = fields[8:10]
    if kind != 2 or machine != 40 or version != 1 or phentsize != 32 or phoff + phnum * 32 > len(raw):
        raise ArmAuditError("Invalid ARM executable/program headers")
    segments = []
    for i in range(phnum):
        typ, offset, virtual, physical, filesz, memsz, flags, alignment = struct.unpack_from("<IIIIIIII", raw, phoff + i * 32)
        if typ != 1:
            continue
        if filesz > memsz or offset + filesz > len(raw) or virtual != physical or physical < 0x40000000 or physical + memsz > 0x48000000:
            raise ArmAuditError("Load segment exceeds diagnostic RAM/file bounds")
        segments.append({"start": virtual, "bytes": memsz, "file_bytes": filesz, "executable": bool(flags & 1), "alignment": alignment})
    validate_entry(entry, segments)
    return entry, segments

def run(*args):
    return subprocess.check_output([str(x) for x in args], text=True)

def definitions(text):
    found = {}
    for line in text.splitlines():
        m = re.search(r"(?:^|:)([0-9a-fA-F]+)\s+([A-Za-z])\s+(\S+)$", line.strip())
        if m and m[2].upper() != "U":
            found[m[3]] = {"address": "0x" + m[1], "kind": m[2], "origin": line[:m.start(1)].rstrip(":")}
    return found

def resolve_toolchain(lock, root):
    if not root:
        raise ArmAuditError("Set XT4_ARM_TOOLCHAIN_ROOT to the verified Arm GNU toolchain")
    root = Path(root).resolve()
    libraries = []
    for relative, identity in lock["libraries"].items():
        path = Path(relative)
        if path.is_absolute() or ".." in path.parts:
            raise ArmAuditError("Library path must stay inside the toolchain root")
        path = (root / path).resolve()
        if root not in path.parents:
            raise ArmAuditError("Library path escapes the toolchain root")
        data = path.read_bytes()
        if len(data) != identity["bytes"] or hashlib.sha256(data).hexdigest() != identity["sha256"]:
            raise ArmAuditError("Toolchain library hash mismatch: " + relative)
        libraries.append(path)
    return root, libraries

def audit(elf, archive, lock_path):
    lock = json.loads(Path(lock_path).read_text())
    root, libraries = resolve_toolchain(lock, os.environ.get("XT4_ARM_TOOLCHAIN_ROOT"))
    prefix = root / "bin/arm-none-eabi-"
    tool = lambda name: str(prefix) + name
    header = run(tool("readelf"), "-h", "-A", elf)
    attrs = {}
    for line in header.splitlines():
        m = re.match(r"\s*([^:]+):\s*(.*?)\s*$", line)
        if m:
            attrs[m[1].strip()] = m[2]
    validate_arm_attributes(attrs)
    entry, segments = read_elf(elf)
    disassembly = run(tool("objdump"), "-d", elf)
    ops = instruction_mnemonics(disassembly)
    if not ops:
        raise ArmAuditError("Empty instruction coverage")
    validate_instructions(ops)
    undefined = [line.split()[-1] for line in run(tool("nm"), "-u", elf).splitlines() if line.strip()]
    defined = definitions(run(tool("nm"), "-n", "--defined-only", elf))
    validate_symbols(undefined, defined)
    archive_u = sorted(set(re.findall(r"\bU\s+(\S+)", run(tool("nm"), "-u", archive))))
    providers = {}
    provider_paths = [str(archive), *[str(p) for p in libraries], *[str(p) for p in Path(elf).parent.glob("CMakeFiles/xt4_face_smoke.dir/arm/*.obj")]]
    for path in provider_paths:
        for name, details in definitions(run(tool("nm"), "-A", "--defined-only", path)).items():
            providers.setdefault(name, []).append(details["origin"] or path)
    unresolved_provider = [name for name in archive_u if name not in providers and name not in defined]
    if unresolved_provider:
        raise ArmAuditError("Missing archive dependency providers: " + ", ".join(unresolved_provider))
    size_text = run(tool("size"), "-A", elf)
    sections = {}
    for line in size_text.splitlines():
        m = re.match(r"^(\.\S+)\s+(\d+)\s+(\d+)", line)
        if m:
            sections[m[1]] = {"bytes": int(m[2]), "address": hex(int(m[3]))}
    init_arrays = sum(v["bytes"] for name, v in sections.items() if name in (".init_array", ".fini_array"))
    if init_arrays:
        raise ArmAuditError("Unexpected C++ global initialization arrays")
    return {"status": "passed", "scope": "standalone ARMv7-A soft-float diagnostic, not camera firmware",
            "elf_sha256": hashlib.sha256(Path(elf).read_bytes()).hexdigest(),
            "archive_sha256": hashlib.sha256(Path(archive).read_bytes()).hexdigest(),
            "entry": hex(entry), "segments": segments, "attributes": attrs, "sections": sections,
            "instructions_examined": len(ops), "undefined_symbols": undefined,
            "archive_undefined_symbols": archive_u, "providers": {x: providers.get(x, ["final ELF"]) for x in archive_u},
            "toolchain_lock_sha256": hashlib.sha256(Path(lock_path).read_bytes()).hexdigest()}

if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("--elf", type=Path, required=True)
    p.add_argument("--archive", type=Path, required=True)
    p.add_argument("--report", type=Path, required=True)
    p.add_argument("--toolchain-lock", type=Path, default=Path(__file__).with_name("toolchain-lock.json"))
    args = p.parse_args()
    try:
        result = audit(args.elf, args.archive, args.toolchain_lock)
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(result, indent=2) + "\n")
        print("ARM audit passed:", result["instructions_examined"], "instructions,", len(result["providers"]), "dependency symbols")
    except (OSError, ArmAuditError, subprocess.CalledProcessError) as error:
        raise SystemExit(str(error))
