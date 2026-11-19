import importlib.util
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("re_kb", ROOT / "tools" / "re" / "kb.py")
KB = importlib.util.module_from_spec(SPEC)
assert SPEC.loader
SPEC.loader.exec_module(KB)


class KnowledgeBaseTests(unittest.TestCase):
    def setUp(self):
        self.records = KB.load_findings()
        self.symbols = KB.load_symbols()

    def test_records_load_with_names_and_confidence(self):
        self.assertGreater(len(self.records), 100)
        for record in self.records:
            self.assertTrue(record.get("name"), record["where"])
            self.assertIn(record.get("confidence"), {"confirmed", "probable", "guess"}, record["where"])

    def test_lint_passes_on_the_checked_in_records(self):
        self.assertEqual(KB.lint(self.records), [])

    def test_lint_rejects_a_disassembly_listing(self):
        listing = {"where": "test", "name": "x", "what":
                   "the body is lwz r3, 4(r31) then addi r4, r3, 8 then stw r4, 0(r31)"}
        self.assertEqual(len(KB.lint([listing])), 1)
        dump = {"where": "test", "name": "x", "how": " ".join(["4E"] * 24)}
        self.assertEqual(len(KB.lint([dump])), 1)

    def test_lint_checks_clean_room_specs(self):
        self.assertEqual(KB.lint_specs(), [])
        fence = "`" * 3
        with tempfile.TemporaryDirectory() as folder:
            spec = Path(folder) / "BAD.md"
            bad = ["# Spec", "", fence + "cpp", "int x;", fence, "", "The loop reads ctx.r3 first.", ""]
            spec.write_text("\n".join(bad), encoding="utf-8")
            problems = KB.lint_specs((Path(folder),))
            self.assertEqual(len(problems), 2, problems)
            good = ["# Spec", "", fence + "text", "[a] -> [b]", fence, ""]
            spec.write_text("\n".join(good), encoding="utf-8")
            self.assertEqual(KB.lint_specs((Path(folder),)), [])

    def test_address_table_names_generated_functions(self):
        table = KB.address_table(self.records, self.symbols)
        self.assertIn("sub_82438EA8", table["names"])  # CRT pow
        self.assertIn("crt pow", table["names"]["sub_82438EA8"].lower())
        addresses = {row["address"] for row in table["addresses"]}
        self.assertIn("0x82438EA8", addresses)

    @unittest.skipUnless((KB.GENERATED / "pinyon_shift_init.cpp").is_file(), "needs the generated tree")
    def test_recorded_functions_are_generated_function_starts(self):
        self.assertEqual(KB.check(self.records, self.symbols, KB.GENERATED, KB.GENERATED_V4), [])


if __name__ == "__main__":
    unittest.main()
