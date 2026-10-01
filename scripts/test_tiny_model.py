#!/usr/bin/env python3
"""Tests for tiny-model.py: the GGUF it writes, read back by a reader of its own.

The script runs as a separate process, as the nightly job runs it, against a
stand-in llama.cpp checkout written here: a vocabulary-only GGUF with as many
tokens as Qwen's (151,936), and a chat template. The reader below is written
apart from the script's writer, so that a mistake in one is not repeated in
the other. That llama-server loads the model is the nightly job's own first
check.

    python3 -m unittest discover -s scripts -p 'test_*.py' -v
"""
from __future__ import annotations

import math
import pathlib
import struct
import subprocess
import sys
import tempfile
import unittest

SCRIPT = pathlib.Path(__file__).with_name("tiny-model.py")
VOCABULARY_SIZE = 151_936
TEMPLATE = "{{ messages }} with <think> when asked\n"

# GGUF value types, and the struct formats of the scalar ones.
STRING, ARRAY = 8, 9
FORMATS = {0: "B", 1: "b", 2: "H", 3: "h", 4: "I", 5: "i", 6: "f", 7: "?", 10: "Q", 11: "q", 12: "d"}


# ----------------------------------------------------------- a stand-in input
def gguf_string(text: str) -> bytes:
    raw = text.encode("utf-8")
    return struct.pack("<Q", len(raw)) + raw


def stand_in_vocabulary(think_ids_free: bool = True) -> bytes:
    """A vocabulary-only GGUF shaped like llama.cpp's models/ggml-vocab-qwen35.gguf."""
    tokens = [f"t{index}" for index in range(151_643)]
    tokens += ["<|endoftext|>", "<|im_start|>", "<|im_end|>"]
    tokens += [f"[PAD{index}]" for index in range(151_646, VOCABULARY_SIZE)]
    if not think_ids_free:
        tokens[151_667] = "taken"
    types = [1] * 151_643 + [3, 3, 3] + [4] * (VOCABULARY_SIZE - 151_646)
    entries = [
        gguf_string("general.architecture") + struct.pack("<I", STRING) + gguf_string("qwen2"),
        gguf_string("tokenizer.ggml.model") + struct.pack("<I", STRING) + gguf_string("gpt2"),
        gguf_string("tokenizer.ggml.pre") + struct.pack("<I", STRING) + gguf_string("qwen35"),
        gguf_string("tokenizer.ggml.tokens") + struct.pack("<IIQ", ARRAY, STRING, len(tokens)) +
        b"".join(gguf_string(token) for token in tokens),
        gguf_string("tokenizer.ggml.token_type") + struct.pack("<IIQ", ARRAY, 5, len(types)) +
        struct.pack(f"<{len(types)}i", *types),
        gguf_string("tokenizer.ggml.merges") + struct.pack("<IIQ", ARRAY, STRING, 1) + gguf_string("t 0"),
        gguf_string("tokenizer.ggml.eos_token_id") + struct.pack("<II", 4, 151_643),
        gguf_string("tokenizer.chat_template") + struct.pack("<I", STRING) + gguf_string("Qwen2's template"),
    ]
    return b"GGUF" + struct.pack("<IQQ", 3, 0, len(entries)) + b"".join(entries)


# --------------------------------------------------------------- the reader
class Gguf:
    """Everything in a GGUF file: metadata, tensor directory, tensor bytes."""

    def __init__(self, data: bytes) -> None:
        self.data = data
        self.at = 0
        assert data[:4] == b"GGUF", "magic"
        self.at = 4
        self.version, tensor_count, kv_count = self.take("<IQQ")
        self.metadata = {}
        for _ in range(kv_count):
            key = self.string()
            (kind,) = self.take("<I")
            self.metadata[key] = self.value(kind)
        self.tensors = []
        for _ in range(tensor_count):
            name = self.string()
            (dims,) = self.take("<I")
            shape = self.take(f"<{dims}Q")
            kind, offset = self.take("<IQ")
            self.tensors.append((name, shape, kind, offset))
        self.data_start = self.at + (-self.at % 32)

    def take(self, fmt: str) -> tuple:
        values = struct.unpack_from(fmt, self.data, self.at)
        self.at += struct.calcsize(fmt)
        return values

    def string(self) -> str:
        (length,) = self.take("<Q")
        text = self.data[self.at:self.at + length].decode("utf-8")
        self.at += length
        return text

    def value(self, kind: int):
        if kind == STRING:
            return self.string()
        if kind == ARRAY:
            element, count = self.take("<IQ")
            return [self.value(element) for _ in range(count)]
        return self.take("<" + FORMATS[kind])[0]

    def tensor_bytes(self, name: str) -> bytes:
        for tensor_name, shape, kind, offset in self.tensors:
            if tensor_name == name:
                size = math.prod(shape) * (4 if kind == 0 else 2)
                start = self.data_start + offset
                return self.data[start:start + size]
        raise KeyError(name)


# ------------------------------------------------------------------- the tests
class TinyModelTest(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.directory.name)
        (self.root / "llama.cpp/models/templates").mkdir(parents=True)
        self.write_inputs()

    def tearDown(self) -> None:
        self.directory.cleanup()

    def write_inputs(self, think_ids_free: bool = True) -> None:
        models = self.root / "llama.cpp/models"
        (models / "ggml-vocab-qwen35.gguf").write_bytes(stand_in_vocabulary(think_ids_free))
        (models / "templates/Qwen3.5-4B.jinja").write_text(TEMPLATE, encoding="utf-8")

    def run_script(self, out: str, *extra: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            [sys.executable, str(SCRIPT), "--llama-cpp", str(self.root / "llama.cpp"),
             "--out", str(self.root / out), *extra],
            capture_output=True, text=True, timeout=120, check=False)

    def model(self, out: str = "tiny.gguf", *extra: str) -> Gguf:
        result = self.run_script(out, *extra)
        self.assertEqual(result.returncode, 0, result.stderr)
        return Gguf((self.root / out).read_bytes())

    def test_the_same_seed_writes_the_same_bytes(self) -> None:
        self.model("a.gguf")
        self.model("b.gguf")
        self.model("c.gguf", "--seed", "2")
        a, b, c = ((self.root / name).read_bytes() for name in ("a.gguf", "b.gguf", "c.gguf"))
        self.assertEqual(a, b, "so the file can be pinned by its checksum")
        self.assertNotEqual(a, c)

    def test_it_is_a_qwen3_model_with_qwen3s_tokens_and_template(self) -> None:
        model = self.model("tiny.gguf", "--context", "4096")
        meta = model.metadata
        self.assertEqual(model.version, 3)
        self.assertEqual(meta["general.architecture"], "qwen3")
        self.assertEqual(meta["qwen3.context_length"], 4096)
        self.assertEqual((meta["qwen3.block_count"], meta["qwen3.embedding_length"],
                          meta["qwen3.attention.head_count"], meta["qwen3.attention.head_count_kv"],
                          meta["qwen3.attention.key_length"], meta["qwen3.feed_forward_length"]),
                         (2, 64, 4, 2, 16, 128))
        self.assertIn("qwen3.attention.layer_norm_rms_epsilon", meta, "llama.cpp requires it for qwen3")
        tokens, types = meta["tokenizer.ggml.tokens"], meta["tokenizer.ggml.token_type"]
        self.assertEqual(len(tokens), VOCABULARY_SIZE)
        self.assertEqual((tokens[151_667], tokens[151_668]), ("<think>", "</think>"))
        self.assertEqual((types[151_667], types[151_668]), (4, 4))
        self.assertEqual(tokens[151_669], "[PAD151669]", "only the two placeholders change")
        self.assertEqual(meta["tokenizer.ggml.pre"], "qwen35", "the tokenizer's keys are copied")
        self.assertEqual(meta["tokenizer.ggml.merges"], ["t 0"])
        self.assertEqual(meta["tokenizer.ggml.eos_token_id"], 151_645, "answers end at <|im_end|>")
        self.assertIs(meta["tokenizer.ggml.add_bos_token"], False)
        self.assertEqual(meta["tokenizer.chat_template"], TEMPLATE, "Qwen3.5's template, not the vocabulary's")

    def test_the_tensors_are_where_llama_cpp_looks_for_them(self) -> None:
        model = self.model()
        shapes = {name: (shape, kind) for name, shape, kind, _ in model.tensors}
        f32, f16 = 0, 1
        expected = {"token_embd.weight": ((64, VOCABULARY_SIZE), f16), "output_norm.weight": ((64,), f32)}
        for layer in range(2):
            block = f"blk.{layer}"
            expected |= {
                f"{block}.attn_norm.weight": ((64,), f32),
                f"{block}.attn_q.weight": ((64, 64), f16),
                f"{block}.attn_k.weight": ((64, 32), f16),
                f"{block}.attn_v.weight": ((64, 32), f16),
                f"{block}.attn_output.weight": ((64, 64), f16),
                f"{block}.attn_q_norm.weight": ((16,), f32),
                f"{block}.attn_k_norm.weight": ((16,), f32),
                f"{block}.ffn_norm.weight": ((64,), f32),
                f"{block}.ffn_gate.weight": ((64, 128), f16),
                f"{block}.ffn_up.weight": ((64, 128), f16),
                f"{block}.ffn_down.weight": ((128, 64), f16),
            }
        self.assertEqual(shapes, expected)

        # Laid out back to back, each aligned, and the file ends with the last.
        end = 0
        for _, shape, kind, offset in model.tensors:
            self.assertEqual(offset % 32, 0)
            self.assertEqual(offset, end)
            size = math.prod(shape) * (4 if kind == 0 else 2)
            end = offset + size + (-size % 32)
        self.assertEqual(len(model.data), model.data_start + end)

    def test_weights_are_small_and_finite_and_norms_are_one(self) -> None:
        model = self.model()
        weights = struct.unpack("<4096e", model.tensor_bytes("blk.0.attn_q.weight"))
        self.assertTrue(all(math.isfinite(value) and 2**-8 <= abs(value) < 2**-4 for value in weights))
        self.assertTrue(any(value < 0 for value in weights) and any(value > 0 for value in weights))
        self.assertEqual(set(struct.unpack("<16f", model.tensor_bytes("blk.1.attn_k_norm.weight"))), {1.0})

    def test_refuses_a_vocabulary_whose_thinking_ids_are_taken(self) -> None:
        self.write_inputs(think_ids_free=False)
        result = self.run_script("tiny.gguf")
        self.assertEqual(result.returncode, 1)
        self.assertIn("token 151667 is 'taken', not a padding placeholder", result.stderr)
        self.assertFalse((self.root / "tiny.gguf").exists())


if __name__ == "__main__":
    unittest.main()
