from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from agent_rag.tokenizer import tokenize


class TokenizerTests(unittest.TestCase):
    def test_tokenizer_handles_identifiers_and_chinese_deterministically(self) -> None:
        tokens = tokenize("HTTPParser parse_json 修复知识库检索")

        self.assertIn("httpparser", tokens)
        self.assertIn("http", tokens)
        self.assertIn("parser", tokens)
        self.assertIn("parse_json", tokens)
        self.assertIn("parse", tokens)
        self.assertIn("json", tokens)
        self.assertIn("知", tokens)
        self.assertIn("知识", tokens)
        self.assertIn("检索", tokens)
        self.assertEqual(tokens, tokenize("HTTPParser parse_json 修复知识库检索"))
