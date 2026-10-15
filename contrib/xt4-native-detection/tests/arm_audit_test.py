import importlib.util
from pathlib import Path
import unittest
spec = importlib.util.spec_from_file_location("check_arm", Path(__file__).resolve().parents[1] / "tools/check_arm.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

class ArmAuditTest(unittest.TestCase):
    def test_rejects_aarch64(self):
        with self.assertRaises(module.ArmAuditError):
            module.validate_arm_attributes({"Class": "ELF64", "Machine": "AArch64"})

    def test_accepts_little_endian_arm_soft(self):
        module.validate_arm_attributes({"Class": "ELF32", "Machine": "ARM", "Data": "2's complement, little endian",
            "Tag_CPU_arch": "v7", "Tag_CPU_arch_profile": "Application"})

    def test_rejects_hard_float(self):
        with self.assertRaises(module.ArmAuditError):
            module.validate_arm_attributes({"Class": "ELF32", "Machine": "ARM", "Data": "2's complement, little endian",
                "Tag_CPU_arch": "v7", "Tag_ABI_VFP_args": "VFP registers"})

    def test_rejects_neon_and_vfp(self):
        for op in ["vadd.f32", "vldr", "vpush"]:
            with self.assertRaises(module.ArmAuditError):
                module.validate_instructions(["mov", op, "bx"])

    def test_decodes_hex_word_without_eating_add_mnemonic(self):
        self.assertEqual(module.instruction_mnemonics(
            "40010000: e2800001  add r0, r0, #1\n40010004: f2020d44  vadd.f32 q0, q1, q2\n"),
            ["add", "vadd.f32"])

    def test_entry_must_be_executable(self):
        with self.assertRaises(module.ArmAuditError):
            module.validate_entry(0x40010000, [{"start": 0x40010000, "bytes": 4096, "executable": False}])
        with self.assertRaises(module.ArmAuditError):
            module.validate_entry(0, [{"start": 0x40010000, "bytes": 4096, "executable": True}])
        module.validate_entry(0x40010000, [{"start": 0x40010000, "bytes": 4096, "executable": True}])

    def test_unresolved_and_heap_dependencies_rejected(self):
        with self.assertRaises(module.ArmAuditError):
            module.validate_symbols(["mystery_runtime"], {"main": {}})
        for name in ["malloc", "_sbrk", "_exit", "_Znwj", "__atomic_load_8"]:
            with self.assertRaises(module.ArmAuditError):
                module.validate_symbols([], {name: {}})

if __name__ == "__main__":
    unittest.main()
