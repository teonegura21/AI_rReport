#!/usr/bin/env python3
"""Write a tiny Qwen3 model, with random weights, for tests against a real llama-server.

The nightly job (.github/workflows/nightly.yml) runs rReport's runtime with
llama-server, built for the CPU at the image's pinned llama.cpp. It checks the
server's mechanics, not a model's skill:
  * a grammar keeps a schema's fields in the order written;
  * n choices report their usage per choice;
  * a thinking budget cuts the thinking short;
  * a shared prefix is served from the cache;
  * each slot gets the context the catalog plans;
  * an overflow fails the way the runtime documents.
None of that needs a trained model, so this script makes one from nothing:
  * The architecture is llama.cpp's "qwen3", at toy size: 2 layers, width
    64, 4 query heads over 2 key/value heads, a feed-forward width of 128.
  * The vocabulary is Qwen's: 151,936 tokens, copied from the vocabulary-only
    file llama.cpp keeps for its tokenizer tests
    (models/ggml-vocab-qwen35.gguf). <think> and </think> are put back at
    Qwen3's ids, 151667 and 151668, over two of its padding placeholders.
  * The chat template is Qwen3.5's (models/templates/Qwen3.5-4B.jinja). With
    thinking on, it opens the thinking block in the prompt itself, so a
    thinking budget can be checked with a model that would never open one.
  * The weights are pseudo-random numbers drawn from --seed. The same seed
    writes the same bytes, so the file can be pinned by its checksum.
Nothing is downloaded: both inputs come from the llama.cpp checkout that
llama-server is built from, at the pinned commit.

  tiny-model.py --llama-cpp DIR --out FILE [--seed N] [--context N]

The file is about 20 MB: matrices in f16, norms in f32. Its trained context
(qwen3.context_length, --context, default 8192) caps a slot's context, as a
real model's does.

The format is GGUF version 3 (llama.cpp's gguf.h): a header, metadata as
typed key/value pairs, tensor descriptions, then the tensors' bytes, each
aligned to 32 bytes. It is written here in plain Python, with no numpy and
no gguf package, so that the job needs nothing installed.
"""
from __future__ import annotations

import argparse
import array
import dataclasses
import enum
import pathlib
import random
import struct
import sys
from typing import Any

MAGIC = b"GGUF"
VERSION = 3
ALIGNMENT = 32  # GGUF's default (general.alignment)

VOCABULARY = pathlib.Path("models/ggml-vocab-qwen35.gguf")
TEMPLATE = pathlib.Path("models/templates/Qwen3.5-4B.jinja")

# Qwen3's thinking tokens, and the placeholders they replace in the
# vocabulary-only file (whose tokenizer predates them).
THINK_TOKENS = {151667: "<think>", 151668: "</think>"}
IM_END = 151645        # <|im_end|>: where Qwen3's turns, and answers, end
END_OF_TEXT = 151643   # <|endoftext|>


class ValueType(enum.IntEnum):
    """GGUF metadata value types (gguf_type)."""
    UINT8 = 0
    INT8 = 1
    UINT16 = 2
    INT16 = 3
    UINT32 = 4
    INT32 = 5
    FLOAT32 = 6
    BOOL = 7
    STRING = 8
    ARRAY = 9
    UINT64 = 10
    INT64 = 11
    FLOAT64 = 12


SCALARS = {
    ValueType.UINT8: "<B", ValueType.INT8: "<b", ValueType.UINT16: "<H", ValueType.INT16: "<h",
    ValueType.UINT32: "<I", ValueType.INT32: "<i", ValueType.FLOAT32: "<f", ValueType.BOOL: "<?",
    ValueType.UINT64: "<Q", ValueType.INT64: "<q", ValueType.FLOAT64: "<d",
}


class TensorType(enum.IntEnum):
    """The ggml tensor types used here (ggml_type)."""
    F32 = 0
    F16 = 1


@dataclasses.dataclass(frozen=True)
class Value:
    """A metadata value with its GGUF type; arrays carry their elements' type."""
    type: ValueType
    value: Any
    element_type: ValueType | None = None


@dataclasses.dataclass(frozen=True)
class Tensor:
    name: str
    shape: tuple[int, ...]  # ggml order: the fastest-varying dimension first
    type: TensorType
    data: bytes


@dataclasses.dataclass(frozen=True)
class Shape:
    """The model's size. The defaults are the smallest that exercise every
    mechanism: grouped-query attention, per-head norms, a gated feed-forward."""
    layers: int = 2
    width: int = 64
    heads: int = 4
    kv_heads: int = 2
    feed_forward: int = 128

    @property
    def head_size(self) -> int:
        return self.width // self.heads


# --------------------------------------------------------------------- reading
class Reader:
    """Reads GGUF metadata (tensors are not needed here)."""

    def __init__(self, data: bytes) -> None:
        self.data = data
        self.position = 0

    def take(self, fmt: str) -> Any:
        (value,) = struct.unpack_from(fmt, self.data, self.position)
        self.position += struct.calcsize(fmt)
        return value

    def string(self) -> str:
        length = self.take("<Q")
        text = self.data[self.position:self.position + length].decode("utf-8")
        self.position += length
        return text

    def value(self, kind: ValueType) -> Value:
        if kind == ValueType.STRING:
            return Value(kind, self.string())
        if kind == ValueType.ARRAY:
            element_type = ValueType(self.take("<I"))
            count = self.take("<Q")
            return Value(kind, [self.value(element_type).value for _ in range(count)], element_type)
        return Value(kind, self.take(SCALARS[kind]))


def read_metadata(path: pathlib.Path) -> dict[str, Value]:
    reader = Reader(path.read_bytes())
    if reader.data[:4] != MAGIC:
        raise ValueError(f"{path}: not a GGUF file")
    reader.position = 4
    version = reader.take("<I")
    if version != VERSION:
        raise ValueError(f"{path}: GGUF version {version}, expected {VERSION}")
    reader.take("<Q")  # tensor count
    count = reader.take("<Q")
    metadata: dict[str, Value] = {}
    for _ in range(count):
        key = reader.string()
        metadata[key] = reader.value(ValueType(reader.take("<I")))
    return metadata


# --------------------------------------------------------------------- writing
def encode_string(text: str) -> bytes:
    raw = text.encode("utf-8")
    return struct.pack("<Q", len(raw)) + raw


def encode_value(value: Value) -> bytes:
    if value.type == ValueType.STRING:
        return encode_string(value.value)
    if value.type == ValueType.ARRAY:
        assert value.element_type is not None
        head = struct.pack("<IQ", value.element_type, len(value.value))
        if value.element_type == ValueType.STRING:
            return head + b"".join(encode_string(item) for item in value.value)
        return head + struct.pack(f"<{len(value.value)}{SCALARS[value.element_type][1]}", *value.value)
    return struct.pack(SCALARS[value.type], value.value)


def padding(size: int) -> int:
    return -size % ALIGNMENT


def write_gguf(path: pathlib.Path, metadata: dict[str, Value], tensors: list[Tensor]) -> None:
    header = MAGIC + struct.pack("<IQQ", VERSION, len(tensors), len(metadata))
    parts = [header]
    for key, value in metadata.items():
        parts.append(encode_string(key) + struct.pack("<I", value.type) + encode_value(value))
    offset = 0
    for tensor in tensors:
        parts.append(encode_string(tensor.name) + struct.pack("<I", len(tensor.shape)) +
                     struct.pack(f"<{len(tensor.shape)}Q", *tensor.shape) +
                     struct.pack("<IQ", tensor.type, offset))
        offset += len(tensor.data) + padding(len(tensor.data))
    described = sum(len(part) for part in parts)
    parts.append(b"\0" * padding(described))
    for tensor in tensors:
        parts.append(tensor.data + b"\0" * padding(len(tensor.data)))
    path.write_bytes(b"".join(parts))


# ------------------------------------------------------------------- the model
def small_halves(rng: random.Random, count: int) -> bytes:
    """`count` random f16 values of magnitude 2^-8 to 2^-4 and either sign.

    Built from bit patterns rather than through floats: a sign bit, one of
    four exponents, a random mantissa. Every pattern is a finite number, and
    the whole tensor takes one pass over random bytes."""
    raw = array.array("H")
    raw.frombytes(rng.randbytes(2 * count))
    # Exponent field 7..10 (biased by 15): 2^-8 to 2^-5, times 1.mantissa.
    values = array.array("H", ((bits & 0x83FF) | ((7 + ((bits >> 10) & 3)) << 10) for bits in raw))
    if sys.byteorder == "big":
        values.byteswap()
    return values.tobytes()


def ones(count: int) -> bytes:
    values = array.array("f", [1.0] * count)
    if sys.byteorder == "big":
        values.byteswap()
    return values.tobytes()


def tensors_for(shape: Shape, vocabulary_size: int, seed: int) -> list[Tensor]:
    rng = random.Random(seed)

    def matrix(name: str, columns: int, rows: int) -> Tensor:
        return Tensor(name, (columns, rows), TensorType.F16, small_halves(rng, columns * rows))

    def norm(name: str, size: int) -> Tensor:
        return Tensor(name, (size,), TensorType.F32, ones(size))

    q_width = shape.heads * shape.head_size
    kv_width = shape.kv_heads * shape.head_size
    # The output projection is the embedding, as in Qwen3's small models: no
    # output.weight, and llama.cpp reuses token_embd.weight.
    tensors = [matrix("token_embd.weight", shape.width, vocabulary_size),
               norm("output_norm.weight", shape.width)]
    for layer in range(shape.layers):
        block = f"blk.{layer}"
        tensors += [
            norm(f"{block}.attn_norm.weight", shape.width),
            matrix(f"{block}.attn_q.weight", shape.width, q_width),
            matrix(f"{block}.attn_k.weight", shape.width, kv_width),
            matrix(f"{block}.attn_v.weight", shape.width, kv_width),
            matrix(f"{block}.attn_output.weight", q_width, shape.width),
            norm(f"{block}.attn_q_norm.weight", shape.head_size),
            norm(f"{block}.attn_k_norm.weight", shape.head_size),
            norm(f"{block}.ffn_norm.weight", shape.width),
            matrix(f"{block}.ffn_gate.weight", shape.width, shape.feed_forward),
            matrix(f"{block}.ffn_up.weight", shape.width, shape.feed_forward),
            matrix(f"{block}.ffn_down.weight", shape.feed_forward, shape.width),
        ]
    return tensors


def metadata_for(vocabulary: dict[str, Value], template: str, shape: Shape, context: int) -> dict[str, Value]:
    tokens = list(vocabulary["tokenizer.ggml.tokens"].value)
    types = list(vocabulary["tokenizer.ggml.token_type"].value)
    for token_id, text in THINK_TOKENS.items():
        if not tokens[token_id].startswith("[PAD"):
            raise ValueError(f"token {token_id} is {tokens[token_id]!r}, not a padding placeholder")
        tokens[token_id] = text
        types[token_id] = 4  # user-defined, as Qwen3's own files mark them

    u32 = lambda number: Value(ValueType.UINT32, number)  # noqa: E731
    f32 = lambda number: Value(ValueType.FLOAT32, number)  # noqa: E731
    metadata = {
        "general.architecture": Value(ValueType.STRING, "qwen3"),
        "general.name": Value(ValueType.STRING, "rreport-tiny-qwen3"),
        "general.file_type": u32(1),  # mostly f16
        "qwen3.context_length": u32(context),
        "qwen3.embedding_length": u32(shape.width),
        "qwen3.block_count": u32(shape.layers),
        "qwen3.feed_forward_length": u32(shape.feed_forward),
        "qwen3.attention.head_count": u32(shape.heads),
        "qwen3.attention.head_count_kv": u32(shape.kv_heads),
        "qwen3.attention.key_length": u32(shape.head_size),
        "qwen3.attention.value_length": u32(shape.head_size),
        "qwen3.rope.freq_base": f32(1_000_000.0),
        "qwen3.attention.layer_norm_rms_epsilon": f32(1e-6),
    }
    for key, value in vocabulary.items():
        if key.startswith("tokenizer.") and key != "tokenizer.chat_template":
            metadata[key] = value
    metadata["tokenizer.ggml.tokens"] = Value(ValueType.ARRAY, tokens, ValueType.STRING)
    metadata["tokenizer.ggml.token_type"] = Value(ValueType.ARRAY, types,
                                                  vocabulary["tokenizer.ggml.token_type"].element_type)
    # As Qwen3's own files: an answer ends at <|im_end|>, and no BOS is added.
    metadata["tokenizer.ggml.eos_token_id"] = u32(IM_END)
    metadata["tokenizer.ggml.bos_token_id"] = u32(END_OF_TEXT)
    metadata["tokenizer.ggml.padding_token_id"] = u32(END_OF_TEXT)
    metadata["tokenizer.ggml.add_bos_token"] = Value(ValueType.BOOL, False)
    metadata["tokenizer.chat_template"] = Value(ValueType.STRING, template)
    return metadata


def write_model(llama_cpp: pathlib.Path, out: pathlib.Path, seed: int, context: int,
                shape: Shape = Shape()) -> None:
    vocabulary = read_metadata(llama_cpp / VOCABULARY)
    template = (llama_cpp / TEMPLATE).read_text(encoding="utf-8")
    size = len(vocabulary["tokenizer.ggml.tokens"].value)
    write_gguf(out, metadata_for(vocabulary, template, shape, context), tensors_for(shape, size, seed))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--llama-cpp", type=pathlib.Path, required=True,
                        help="a llama.cpp checkout (its models/ directory is read)")
    parser.add_argument("--out", type=pathlib.Path, required=True, help="the GGUF file to write")
    parser.add_argument("--seed", type=int, default=1, help="the weights' seed (default 1)")
    parser.add_argument("--context", type=int, default=8192,
                        help="the trained context, which caps a slot's (default 8192)")
    args = parser.parse_args(argv)
    try:
        write_model(args.llama_cpp, args.out, args.seed, args.context)
    except (OSError, ValueError, KeyError) as error:
        print(f"tiny-model: {error}", file=sys.stderr)
        return 1
    print(f"wrote {args.out} ({args.out.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
