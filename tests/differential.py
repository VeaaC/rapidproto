#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Christian Vetter
"""Randomized differential test: decode the same bytes with RapidProto and with protobuf, and
compare every field.

protobuf is the oracle. For each schema it builds random messages through reflection, serializes
them, and hands the bytes to the generated arena decoder; the decoded tree comes back as text from
the debug dumper, and both sides are canonicalized against the descriptor and compared. A schema
needs no hand-written comparator -- `rapidproto_diffgen` emits the C++ harness -- so this scales to
any schema protoc and rapidprotoc both accept, and whose generated header a program with an
`int main()` can include (which rules out `package main`; C++ has one namespace for both).

Why the dumper is the comparison surface: RapidProto has no reflection, so something has to turn a
decoded tree into data a script can walk, and the dumper is the only thing that does. It reads
through the same public accessors a consumer would, so a wrong decoded VALUE still shows up here;
only a dumper formatting bug could produce a false mismatch, and those the golden tests pin.

Payloads are not only what protobuf's serializer emits: any payload the wire shuffler can change
is also decoded in its shuffled form (records permuted, packed <-> expanded re-encoded, a
singular scalar duplicated -- see the wire-shape mutation section), so out-of-order tags, both
packings and last-one-wins get their VALUES compared here rather than only their crash-safety
fuzzed.

What this cannot cover, by construction:
  - extensions, which RapidProto never materializes -- the generator below skips them
  - unknown fields, since payloads are built from the same schema that decodes them
  - the wire shapes the mutation section leaves alone (map-entry body order, duplicated LEN
    records, non-minimal varints ...) -- see there for which are fuzzer-covered and which are
    excluded as documented divergences

Editions schemas skip unless the local protoc and protobuf bindings are new enough for them
(editions went GA in protoc v27), and that is accepted rather than worked around. Editions do not
add decode BEHAVIOUR -- they re-express the existing behaviours as features (field_presence,
repeated_field_encoding, message_encoding, enum_type), and every one of those decode paths is
already covered here through the proto2 and proto3 fixtures. What is editions-specific is which
behaviour a field resolves to, which is a front-end question the goldens pin and the corpus gate
sweeps against protobuf's own editions conformance schemas.

Requires protoc and the protobuf Python bindings; skips cleanly when either is missing.

Usage:
    python3 tests/differential.py                       # every corpus schema, default seed
    python3 tests/differential.py --messages 200        # more payloads per message type
    python3 tests/differential.py --seed 7 --verbose
    python3 tests/differential.py --schema tests/corpus/proto3.proto
    python3 tests/differential.py --jobs 4                # schemas checked in parallel
    python3 tests/differential.py --write-seeds build/fuzz/payload-seeds   # seed the fuzzers
"""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import math
import random
import shutil
import struct
import os
import subprocess
import sys
import tempfile
from itertools import repeat
from pathlib import Path

# Reserved exit code for "I could not run, and that is expected" -- check.sh reports it as a
# self-skip rather than a pass, so a green gate cannot quietly have checked nothing.
SKIP_RC = 77

REPO = Path(__file__).resolve().parent.parent
CORPUS = REPO / "tests" / "corpus"
DEFAULT_BUILD_DIR = REPO / "build" / "gcc"

# Per message type. Enough that the random walk reaches most field combinations without making the
# per-schema C++ compile (the dominant cost) look cheap by comparison.
DEFAULT_MESSAGES = 40

# How deep a randomly built message may nest. Self-referential schemas (`Msg { Msg self = 5; }`) are
# ordinary, so recursion needs a bound; past it, sub-message fields are simply left unset.
MAX_DEPTH = 4

# Scalar edge cases worth hitting far more often than a uniform draw would. Decoders break at the
# boundaries -- varint width changes, sign handling, the zigzag mapping -- not in the middle.
EDGE_INTS = [0, 1, -1, 2, 127, 128, 255, 256, 16383, 16384, 65535, 2**31 - 1, -(2**31), 2**31,
             2**32 - 1, 2**63 - 1, -(2**63), 2**64 - 1]
EDGE_FLOATS = [0.0, -0.0, 1.0, -1.0, 0.5, 0.1, 3.141592653589793, 1e-300, 1e300,
               float("nan"), float("inf"), float("-inf"), 5e-324, 1.5e-45]
EDGE_STRINGS = ["", "a", "hi\nthere", 'quote"backslash\\', "éé\U0001F600", "\x01\x02",
                "x" * 200]
EDGE_BYTES = [b"", b"\x00", b"\x00\x01\xff", b"\xff" * 100, bytes(range(256))]


def is_message(field) -> bool:
    """Whether a field holds a sub-message. A proto2 GROUP is one -- same nesting, different wire
    encoding -- and protobuf's descriptor gives it its own type, so every recursion checks both."""
    from google.protobuf.descriptor import FieldDescriptor as FD
    return field.type in (FD.TYPE_MESSAGE, FD.TYPE_GROUP)


def is_open_enum(enum_type) -> bool:
    """Whether an unknown number stays in the field rather than moving to unknown fields.

    Proto3 only. Editions files report `syntax == "editions"`, so an editions enum declaring OPEN
    is treated as closed; this gates only GENERATION of an out-of-range enum number, so the cost is
    lost coverage, not a false mismatch. It is not editions support. The `getattr` default is the
    same trade in the other direction: if a future binding drops the attribute, every enum reads as
    closed and this coverage disappears silently."""
    return getattr(enum_type.file, "syntax", "proto2") == "proto3"


def is_map(field) -> bool:
    from google.protobuf.descriptor import FieldDescriptor as FD
    return field.type == FD.TYPE_MESSAGE and field.message_type.GetOptions().map_entry


class Skip(Exception):
    """A schema this test cannot drive (protoc or rapidprotoc rejects it, or it has no messages)."""


class HarnessError(Exception):
    """The generated harness did not compile. Reported as a failure, never swallowed: a schema whose
    decoder stops building is exactly what this test exists to notice."""


def first_error(stderr: str) -> str:
    """The first line of `stderr` that is a diagnostic rather than a warning.

    protoc prefixes its warnings with `[libprotobuf WARNING ...]` and prints them BEFORE the error,
    so taking line one verbatim would report a stale-syntax warning as the reason a schema was
    skipped -- naming something that is not why it failed."""
    for line in stderr.strip().splitlines():
        if not line.lstrip().startswith("[libprotobuf WARNING"):
            return line.strip()
    return stderr.strip().splitlines()[0].strip() if stderr.strip() else "(no diagnostic)"


# ── random message construction ──────────────────────────────────────────────────────────────────


def _random_scalar(field, rng: random.Random):
    """A value for one scalar field, biased toward the type's edge cases."""
    from google.protobuf.descriptor import FieldDescriptor as FD

    kind = field.type
    if kind == FD.TYPE_BOOL:
        return rng.random() < 0.5
    if kind == FD.TYPE_STRING:
        return rng.choice(EDGE_STRINGS) if rng.random() < 0.7 else "s%d" % rng.randrange(1000)
    if kind == FD.TYPE_BYTES:
        return rng.choice(EDGE_BYTES) if rng.random() < 0.7 else bytes(
            rng.randrange(256) for _ in range(rng.randrange(8)))
    if kind in (FD.TYPE_FLOAT, FD.TYPE_DOUBLE):
        value = rng.choice(EDGE_FLOATS) if rng.random() < 0.6 else rng.uniform(-1e6, 1e6)
        if kind == FD.TYPE_FLOAT:
            # Round-trip through float32 so the oracle holds exactly what the wire will carry. The
            # edge-case pool spans double's range, so a value past float's overflows to an infinity
            # -- which protobuf stores as such, and is worth generating anyway.
            try:
                value = struct.unpack("<f", struct.pack("<f", value))[0]
            except OverflowError:
                value = math.copysign(float("inf"), value)
        return value
    if kind == FD.TYPE_ENUM:
        # proto3 enums are OPEN: a number no enumerator carries stays in the field rather than
        # becoming an unknown field, and both sides must agree on it -- the dumper renders it
        # UNKNOWN(<n>). Generated only for open enums; a proto2 (closed) enum rejects the value.
        if rng.random() < 0.15 and is_open_enum(field.enum_type):
            return rng.choice([7, 99, -3, 2**31 - 1, -(2**31)])
        return rng.choice([v.number for v in field.enum_type.values])

    # Integers: pick an edge case, then clamp into the field's own range.
    signed = kind in (FD.TYPE_INT32, FD.TYPE_INT64, FD.TYPE_SINT32, FD.TYPE_SINT64,
                      FD.TYPE_SFIXED32, FD.TYPE_SFIXED64)
    bits = 64 if kind in (FD.TYPE_INT64, FD.TYPE_UINT64, FD.TYPE_SINT64, FD.TYPE_FIXED64,
                          FD.TYPE_SFIXED64) else 32
    low, high = (-(2 ** (bits - 1)), 2 ** (bits - 1) - 1) if signed else (0, 2**bits - 1)
    value = rng.choice(EDGE_INTS) if rng.random() < 0.7 else rng.randrange(low, high + 1)
    return max(low, min(high, value))


def fill_random(message, rng: random.Random, depth: int = 0) -> None:
    """Populate `message` in place. Required fields are always set (protobuf refuses to serialize
    otherwise); everything else is set at random, which is what exercises presence handling."""
    from google.protobuf.descriptor import FieldDescriptor as FD

    descriptor = message.DESCRIPTOR
    # At most one member of a oneof may be set, so choose per oneof rather than per field.
    chosen_oneof = {}
    for oneof in descriptor.oneofs:
        chosen_oneof[oneof.name] = rng.choice(oneof.fields) if rng.random() < 0.7 else None

    for field in descriptor.fields:
        if field.containing_oneof is not None:
            if chosen_oneof.get(field.containing_oneof.name) is not field:
                continue
        elif field.label != FD.LABEL_REQUIRED and rng.random() < 0.3:
            continue  # leave it unset

        if is_map(field):
            key_field = field.message_type.fields_by_name["key"]
            value_field = field.message_type.fields_by_name["value"]
            container = getattr(message, field.name)
            for _ in range(rng.randrange(4)):
                key = _random_scalar(key_field, rng)
                if is_message(value_field):
                    if depth < MAX_DEPTH:
                        fill_random(container[key], rng, depth + 1)
                    else:
                        container[key].SetInParent()
                else:
                    container[key] = _random_scalar(value_field, rng)
            continue

        if field.label == FD.LABEL_REPEATED:
            container = getattr(message, field.name)
            for _ in range(rng.randrange(5)):
                if is_message(field):
                    element = container.add()
                    if depth < MAX_DEPTH:
                        fill_random(element, rng, depth + 1)
                else:
                    container.append(_random_scalar(field, rng))
            continue

        if is_message(field):
            sub = getattr(message, field.name)
            # Mark it present explicitly. Filling may set no subfield at all, and protobuf treats a
            # sub-message as absent until something inside it is touched -- which would leave a
            # REQUIRED one unset (unserializable). It also means empty-but-present sub-messages get
            # generated, which is a case worth covering.
            sub.SetInParent()
            if depth < MAX_DEPTH:
                fill_random(sub, rng, depth + 1)
            continue

        setattr(message, field.name, _random_scalar(field, rng))


# ── wire-shape mutation ──────────────────────────────────────────────────────────────────────────
#
# protobuf's serializer emits ONE canonical shape: fields in tag order, each repeated field in its
# declared packing, no tag repeated for a singular slot. Feeding only that would leave the arena
# decoder's in-order fast path as the only path this test value-compares. So every payload is also
# checked in a shuffled variant that any protobuf parser must accept with identical semantics:
#   - one singular scalar record duplicated, the perturbed copy inserted BEFORE the original --
#     so last-one-wins is load-bearing, not vacuously satisfied by equal values
#   - repeated scalar fields re-encoded packed <-> expanded (parsers must accept either form
#     regardless of the declared packing)
#   - the top-level records permuted, preserving relative order WITHIN each field number -- that
#     order is semantics: repeated element order, and which duplicate wins
# The oracle needs no changes; it re-parses whatever bytes it is handed. Mutating the top level
# covers nested messages too -- a nested message shares its decode loop with the top-level entry
# point check_message drives for its type -- with two exceptions: map-ENTRY bodies are dispatch
# loops of their own, inlined into the parent's decoder, so value-before-key, duplicated keys and
# absent halves stay with the fuzzers; and imported types that are no corpus schema of their own
# (usewkt.proto's well-known types) are never driven as a top level, so their loops see only
# canonical bytes. Also left to the fuzzers: duplicated string/bytes records, cross-member oneof
# duplicates, and non-minimal varints.
#
# Two duplications are excluded because they would MANUFACTURE a documented decode divergence
# (docs/semantics.md), not add coverage -- the mutation must not produce records whose meaning
# the two sides define differently, even where the duplicate loses. Enum records: an unknown
# number in a closed enum becomes an unknown field in protobuf, while RapidProto treats every
# enum as open. Singular sub-message records: protobuf merges the duplicates, RapidProto rejects
# them. (Packed<->expanded re-encoding preserves every element byte-for-byte, so enums do take
# part in that.)


def _read_varint(buf: bytes, pos: int) -> tuple[int, int]:
    result = shift = 0
    while True:
        byte = buf[pos]
        pos += 1
        result |= (byte & 0x7F) << shift
        if not byte & 0x80:
            return result, pos
        shift += 7


def _encode_varint(value: int) -> bytes:
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        out.append(byte | (0x80 if value else 0))
        if not value:
            return bytes(out)


def _record_end(buf: bytes, pos: int, wire: int) -> int:
    """The end of one record's value starting at `pos`. Input comes from protobuf's serializer,
    so malformed framing is a bug here, not input to tolerate."""
    if wire == 0:
        return _read_varint(buf, pos)[1]
    if wire == 1:
        return pos + 8
    if wire == 5:
        return pos + 4
    if wire == 2:
        length, pos = _read_varint(buf, pos)
        return pos + length
    if wire == 3:  # group: records until the matching end-group, nesting included
        while True:
            tag, pos = _read_varint(buf, pos)
            if tag & 7 == 4:
                return pos
            pos = _record_end(buf, pos, tag & 7)
    raise ValueError(f"unexpected wire type {wire}")


def wire_records(buf: bytes) -> list[tuple[int, int, bytes]]:
    """Split serialized bytes into top-level (field_number, wire_type, record_bytes) records;
    record_bytes includes the tag."""
    records, pos = [], 0
    while pos < len(buf):
        tag, value_start = _read_varint(buf, pos)
        end = _record_end(buf, value_start, tag & 7)
        records.append((tag >> 3, tag & 7, buf[pos:end]))
        pos = end
    return records


def _dup_singular_scalar(records, descriptor, rng: random.Random):
    """Insert a value-perturbed copy of one singular numeric record before the original."""
    from google.protobuf.descriptor import FieldDescriptor as FD

    fields = {f.number: f for f in descriptor.fields}
    candidates = []
    for index, (number, wire, _) in enumerate(records):
        field = fields.get(number)
        if (wire in (0, 1, 5) and field is not None
                and field.label != FD.LABEL_REPEATED and field.type != FD.TYPE_ENUM):
            candidates.append(index)
    if not candidates:
        return records
    index = rng.choice(candidates)
    number, wire, raw = records[index]
    _, value_start = _read_varint(raw, 0)
    if wire == 0:
        value, _ = _read_varint(raw, value_start)
        # ^1 keeps the value in every integer field's range (and flips a serialized bool cleanly).
        mutated = raw[:value_start] + _encode_varint(value ^ 1)
    else:  # fixed32/64: flip the low bit of the last value byte
        mutated = raw[:-1] + bytes([raw[-1] ^ 1])
    return records[:index] + [(number, wire, mutated)] + records[index:]


def _split_packed(payload: bytes, elem_wire: int) -> list[bytes]:
    pieces, pos = [], 0
    size = {1: 8, 5: 4}.get(elem_wire)
    while pos < len(payload):
        end = _read_varint(payload, pos)[1] if size is None else pos + size
        pieces.append(payload[pos:end])
        pos = end
    return pieces


def _flip_packing(records, descriptor, rng: random.Random):
    """Re-encode ~half the repeated packable scalar fields present: packed records split into
    per-element records, expanded records merged into one packed record at the first's position."""
    from google.protobuf.descriptor import FieldDescriptor as FD

    fixed = {FD.TYPE_FLOAT: 5, FD.TYPE_FIXED32: 5, FD.TYPE_SFIXED32: 5,
             FD.TYPE_DOUBLE: 1, FD.TYPE_FIXED64: 1, FD.TYPE_SFIXED64: 1}
    flip = {}  # field number -> that field's scalar element wire type
    for field in descriptor.fields:
        if (field.label == FD.LABEL_REPEATED and field.type != FD.TYPE_STRING
                and field.type != FD.TYPE_BYTES and not is_message(field)
                and rng.random() < 0.5):
            flip[field.number] = fixed.get(field.type, 0)
    elements = {}  # expanded records' value bytes per flipped field, in order
    for number, wire, raw in records:
        if number in flip and wire != 2:
            elements.setdefault(number, []).append(raw[_read_varint(raw, 0)[1]:])
    out, packed_done = [], set()
    for number, wire, raw in records:
        elem_wire = flip.get(number)
        if elem_wire is None:
            out.append((number, wire, raw))
        elif wire == 2:  # packed -> expanded
            _, pos = _read_varint(raw, 0)
            length, pos = _read_varint(raw, pos)
            tag = _encode_varint(number << 3 | elem_wire)
            for piece in _split_packed(raw[pos:pos + length], elem_wire):
                out.append((number, elem_wire, tag + piece))
        elif number not in packed_done:  # expanded -> packed
            packed_done.add(number)
            payload = b"".join(elements[number])
            out.append((number, 2,
                        _encode_varint(number << 3 | 2) + _encode_varint(len(payload)) + payload))
    return out


def _permute_records(records, rng: random.Random):
    order = list(range(len(records)))
    rng.shuffle(order)
    permuted = [records[i] for i in order]
    # Restore relative order within each field number: same-field records land in whatever
    # positions the shuffle gave that field, but in their original sequence.
    positions: dict[int, list[int]] = {}
    for slot, (number, _, _) in enumerate(permuted):
        positions.setdefault(number, []).append(slot)
    originals: dict[int, list] = {}
    for record in records:
        originals.setdefault(record[0], []).append(record)
    for number, slots in positions.items():
        for slot, record in zip(slots, originals[number]):
            permuted[slot] = record
    return permuted


def shuffle_wire(encoded: bytes, descriptor, rng: random.Random) -> bytes:
    records = wire_records(encoded)
    records = _dup_singular_scalar(records, descriptor, rng)
    records = _flip_packing(records, descriptor, rng)
    return b"".join(raw for _, _, raw in _permute_records(records, rng))


# ── canonical forms ──────────────────────────────────────────────────────────────────────────────


def canon_float(value: float, is32: bool):
    """A float as something comparable. Non-finite values become the same strings the dumper emits;
    finite ones become their BIT PATTERN, so -0.0 and 0.0 stay distinct."""
    if math.isnan(value):
        return "NaN"
    if math.isinf(value):
        return "Infinity" if value > 0 else "-Infinity"
    if is32:
        value = struct.unpack("<f", struct.pack("<f", value))[0]
    return struct.pack("<d", value)


def map_key_text(key) -> str:
    """A map key as the dumper renders it: always a JSON object key, so always a string."""
    if isinstance(key, bool):
        return "true" if key else "false"
    return str(key)


def canon_protobuf(message):
    """Canonical form of a protobuf message, keyed by field name.

    Built from ListFields(), which yields exactly the fields protobuf considers PRESENT -- the same
    rule the dumper prints by (implicit-presence defaults and empty repeated/maps omitted, an
    explicitly-set default kept)."""
    from google.protobuf.descriptor import FieldDescriptor as FD

    out = {}
    for field, value in message.ListFields():
        is32 = field.type == FD.TYPE_FLOAT

        def scalar(raw):
            if field.type == FD.TYPE_ENUM:
                return int(raw)
            if field.type in (FD.TYPE_FLOAT, FD.TYPE_DOUBLE):
                return canon_float(raw, is32)
            if is_message(field):
                return canon_protobuf(raw)
            return raw

        if is_map(field):
            value_field = field.message_type.fields_by_name["value"]
            value_is32 = value_field.type == FD.TYPE_FLOAT
            entries = {}
            for key, entry in value.items():
                if is_message(value_field):
                    entries[map_key_text(key)] = canon_protobuf(entry)
                elif value_field.type == FD.TYPE_ENUM:
                    entries[map_key_text(key)] = int(entry)
                elif value_field.type in (FD.TYPE_FLOAT, FD.TYPE_DOUBLE):
                    entries[map_key_text(key)] = canon_float(entry, value_is32)
                else:
                    entries[map_key_text(key)] = entry
            out[field.name] = entries
        elif field.label == FD.LABEL_REPEATED:
            out[field.name] = [scalar(element) for element in value]
        else:
            out[field.name] = scalar(value)
    return out


def canon_dump_scalar(value, field, enum_texts):
    """One dumped scalar in canonical form. Numbers arrive as their raw JSON TEXT (see parse_dump),
    which is what keeps `-0` a negative zero and a 64-bit value exact."""
    from google.protobuf.descriptor import FieldDescriptor as FD

    if is_message(field):
        return canon_dump(value, field.message_type, enum_texts)
    if field.type == FD.TYPE_ENUM:
        return enum_number(value, field.enum_type, enum_texts)
    if field.type in (FD.TYPE_FLOAT, FD.TYPE_DOUBLE):
        # Non-finite values arrive as the JSON strings "NaN" / "Infinity" / "-Infinity".
        text = value if isinstance(value, str) else str(value)
        return canon_float(float(text.replace("Infinity", "inf")), field.type == FD.TYPE_FLOAT)
    if field.type == FD.TYPE_BYTES:
        return bytes.fromhex(value)
    if field.type == FD.TYPE_BOOL:
        return bool(value)
    if field.type == FD.TYPE_STRING:
        return value
    return int(value)


def canon_dump(node, descriptor, enum_texts):
    """Canonical form of the dumper's JSON for a message, keyed by field name.

    Every value routes through canon_dump_scalar, so the singular, repeated and map-value paths
    cannot drift apart in how they read a type."""
    from google.protobuf.descriptor import FieldDescriptor as FD

    out = {}
    for name, raw in node.items():
        field = descriptor.fields_by_name.get(name)
        if field is None:
            raise AssertionError("dump names a field %r absent from the descriptor" % name)
        if is_map(field):
            value_field = field.message_type.fields_by_name["value"]
            out[name] = {key: canon_dump_scalar(value, value_field, enum_texts)
                         for key, value in raw.items()}
        elif field.label == FD.LABEL_REPEATED:
            out[name] = [canon_dump_scalar(element, field, enum_texts) for element in raw]
        else:
            out[name] = canon_dump_scalar(raw, field, enum_texts)
    return out


def parse_dump(line: str):
    """Parse one dumped message, keeping every number as its raw text.

    Python's JSON reader would turn `-0` into the integer 0 -- dropping a sign the decoder is being
    tested on -- and would render a large uint64 as a float. Holding the lexeme defers both
    decisions to canon_dump_scalar, which knows the field's type."""
    return json.loads(line, parse_float=str, parse_int=str)


def enum_number(text: str, enum_type, enum_texts) -> int:
    """The number behind an enum value as the dumper printed it."""
    if text.startswith("UNKNOWN(") and text.endswith(")"):
        return int(text[len("UNKNOWN("):-1])  # a value no enumerator carries (open enums)
    # RapidProto writes a fully-qualified name with a leading dot (".p2.Color"); protobuf's
    # descriptors do not. The sidecar uses RapidProto's form.
    table = enum_texts.get("." + enum_type.full_name)
    if table is None:
        raise AssertionError("no rendered-name table for enum %s" % enum_type.full_name)
    for number, rendered in table.items():
        if rendered == text:
            return int(number)
    raise AssertionError("enum %s has no value rendering as %r" % (enum_type.full_name, text))


# ── per-schema driver ────────────────────────────────────────────────────────────────────────────


def run(command: list[str], **kwargs) -> subprocess.CompletedProcess:
    # surrogateescape, not strict: a decoder bug can produce a dump that is not valid UTF-8 (a
    # truncated string, say). That must surface as a field difference, not as a codec traceback that
    # aborts the sweep before the remaining schemas run.
    return subprocess.run(command, capture_output=True, text=True, errors="surrogateescape",
                          **kwargs)


def build_schema(schema: Path, work: Path, tools: dict[str, Path], cxx: str):
    """Generate + compile everything one schema needs. Returns (harness, descriptor pool, meta)."""
    from google.protobuf import descriptor_pb2, descriptor_pool

    include = str(schema.parent)
    descriptor_set = work / "schema.desc"
    # --include_imports so the set is self-contained: the pool below has to resolve every imported
    # type (a schema using a well-known type, or any of the cross-file fixtures) to build at all.
    protoc = run([str(tools["protoc"]), f"--proto_path={include}", "--include_imports",
                  f"--descriptor_set_out={descriptor_set}", str(schema)])
    if protoc.returncode != 0:
        raise Skip("protoc rejects it: %s" % first_error(protoc.stderr))

    pool = descriptor_pool.DescriptorPool()
    file_set = descriptor_pb2.FileDescriptorSet()
    file_set.ParseFromString(descriptor_set.read_bytes())
    for file_proto in file_set.file:
        pool.Add(file_proto)

    # A schema whose package starts with `main` generates `namespace main`, and C++ forbids a
    # namespace and a function of the same name in one scope -- so no program with an `int main()`
    # can include that header, this harness included. An inherent conflict, not a decode question.
    entry_package = next((f.package for f in file_set.file if f.name == schema.name), "")
    if entry_package.split(".")[0] == "main":
        raise Skip("package `main` cannot coexist with a program's int main()")

    generated = run([str(tools["rapidprotoc"]), "--arena", "--dump", f"-I{include}",
                     "--out-dir", str(work), str(schema)])
    if generated.returncode != 0:
        # NOT a Skip: protoc accepted this schema, so our own front-end rejecting it is a
        # regression that would otherwise leave coverage silently, one reassuring line at a time.
        raise HarnessError("rapidprotoc rejects a protoc-valid schema: %s"
                           % first_error(generated.stderr))

    harness_source = work / "harness.cpp"
    meta_path = work / "meta.json"
    emitted = run([str(tools["diffgen"]), f"-I{include}", "--out", str(harness_source),
                   "--meta", str(meta_path), str(schema)])
    if emitted.returncode != 0:
        # Same reasoning: diffgen is ours, so its failure is a defect, not a property of the schema.
        raise HarnessError("diffgen failed: %s" % first_error(emitted.stderr))
    meta = json.loads(meta_path.read_text())
    if not meta["messages"]:
        raise Skip("no messages")

    harness = work / "harness"
    # Only the out-dir on the include path: rapidprotoc copies the runtime headers there, so what
    # gets compiled is exactly the self-contained output a consumer receives. Adding the repo's
    # own include/ as well would put the same header behind two paths, which `#pragma once` cannot
    # collapse -- every runtime type would be defined twice.
    compiled = run([cxx, "-std=c++17", "-O0", "-I", str(work),
                    str(harness_source), "-o", str(harness)])
    if compiled.returncode != 0:
        raise HarnessError("harness does not compile:\n%s" % compiled.stderr[:2000])

    return harness, pool, meta


def check_message(harness: Path, work: Path, factory, descriptor, meta, count: int,
                  rng: random.Random, seed_dir: Path | None = None) -> list[str]:
    """Round-trip `count` random messages of one type. Returns a list of mismatch reports."""
    message_class = factory.GetPrototype(descriptor)
    payload_file = work / "payloads.bin"
    blob = bytearray()
    expected = []
    for _ in range(count):
        message = message_class()
        fill_random(message, rng)
        try:
            encoded = message.SerializeToString()
        except Exception as error:  # noqa: BLE001 - report and move on
            return ["%s: protobuf could not serialize a generated message: %s"
                    % (descriptor.full_name, error)]
        # A payload the wire shuffler can change is decoded twice: as serialized, and shuffled
        # (see the mutation section above) -- otherwise the canonical shape is the only one whose
        # VALUES are ever compared. Single-record payloads often shuffle to themselves; decoding
        # identical bytes twice proves nothing, so those variants are dropped.
        try:
            shuffled = shuffle_wire(encoded, descriptor, rng) if encoded else encoded
        except Exception as error:  # noqa: BLE001 - a crashed wire mutator must not kill the sweep
            return ["%s: the wire mutator crashed: %r\n    payload: %s"
                    % (descriptor.full_name, error, encoded.hex())]
        variants = [("serialized", encoded)]
        if shuffled != encoded:
            variants.append(("shuffled", shuffled))
        for label, payload in variants:
            blob += struct.pack("<I", len(payload)) + payload
            # The oracle is protobuf's own DECODE of these bytes, not the message we filled in:
            # both sides then answer the same question, a protobuf round-trip disagreement cannot
            # pass for agreement -- and the mutator gets no say in what its shuffle "should" mean.
            try:
                expected.append((message_class.FromString(payload), payload, label))
            except Exception as error:  # noqa: BLE001 - a broken mutator must name itself
                if label == "serialized":
                    raise  # protobuf rejecting its own serializer's bytes is not ours to classify
                return ["%s: protobuf rejected a shuffled payload -- the wire mutator broke "
                        "framing: %s\n    payload: %s"
                        % (descriptor.full_name, error, payload.hex())]
    payload_file.write_bytes(bytes(blob))
    if seed_dir is not None:
        # One file per payload, for use as a fuzzer seed corpus (see check.sh's fuzz smoke). These
        # are valid messages over real schemas -- the shuffled variants included, so non-canonical
        # shapes seed the fuzzers too -- which is what a mutation-based fuzzer needs to reach
        # decoder arms it would otherwise take a very long time to stumble into.
        for index, (_, encoded, _) in enumerate(expected):
            (seed_dir / f"{descriptor.full_name}.{index}.bin").write_bytes(encoded)

    result = run([str(harness), "." + descriptor.full_name, str(payload_file)])
    if result.returncode != 0:
        return ["%s: harness failed: %s" % (descriptor.full_name, result.stderr.strip())]
    lines = result.stdout.splitlines()
    if len(lines) != len(expected):
        return ["%s: harness printed %d dumps for %d payloads"
                % (descriptor.full_name, len(lines), len(expected))]

    failures = []
    for index, (line, (message, encoded, label)) in enumerate(zip(lines, expected)):
        if line == "!decode-failed":
            failures.append("%s #%d (%s): RapidProto rejected bytes protobuf accepts"
                            "\n    payload: %s"
                            % (descriptor.full_name, index, label, encoded.hex()))
            continue
        try:
            dumped = canon_dump(parse_dump(line), descriptor, meta["enums"])
        except (ValueError, AssertionError) as error:
            failures.append("%s #%d (%s): dump not usable (%s)\n    dump: %s"
                            % (descriptor.full_name, index, label, error, line[:400]))
            continue
        wanted = canon_protobuf(message)
        if dumped != wanted:
            failures.append("%s #%d (%s): fields differ\n    %s\n    payload: %s"
                            % (descriptor.full_name, index, label,
                               describe_difference(wanted, dumped), encoded.hex()))
    return failures


def describe_difference(wanted, got, path: str = "") -> str:
    """The first differing field, as a path plus both values."""
    if isinstance(wanted, dict) and isinstance(got, dict):
        for key in sorted(set(wanted) | set(got)):
            where = f"{path}.{key}" if path else key
            if key not in got:
                return f"{where}: protobuf={wanted[key]!r} rapidproto=<absent>"
            if key not in wanted:
                return f"{where}: protobuf=<absent> rapidproto={got[key]!r}"
            if wanted[key] != got[key]:
                return describe_difference(wanted[key], got[key], where)
    elif isinstance(wanted, list) and isinstance(got, list):
        if len(wanted) != len(got):
            return f"{path}: protobuf has {len(wanted)} elements, rapidproto {len(got)}"
        for index, (a, b) in enumerate(zip(wanted, got)):
            if a != b:
                return describe_difference(a, b, f"{path}[{index}]")
    return f"{path or '<root>'}: protobuf={wanted!r} rapidproto={got!r}"


def check_schema(schema: Path, tools: dict[str, Path], cxx: str, seed: int, messages: int,
                 seed_dir: Path | None) -> tuple[int, str | None, list[str]]:
    """One schema end to end: (messages checked, skip reason or None, mismatch descriptions).

    Runs in a worker process, so every argument is picklable and nothing is shared but `seed_dir`
    (whose files are named after the message FQN, unique across schemas).
    """
    from google.protobuf import message_factory  # re-imported here: this runs in a worker process

    rng = random.Random(f"{seed}:{schema.name}")  # per schema, so one file's set is stable
    with tempfile.TemporaryDirectory(prefix="rpdiff-") as directory:
        work = Path(directory)
        try:
            harness, pool, meta = build_schema(schema, work, tools, cxx)
        except Skip as reason:
            return 0, f"{schema.name}: {reason}", []
        except HarnessError as error:
            return 0, None, [f"{schema.name}: {error}"]
        factory = message_factory.MessageFactory(pool)
        checked = 0
        failures: list[str] = []
        for fqn in meta["messages"]:
            descriptor = pool.FindMessageTypeByName(fqn.lstrip("."))
            if descriptor.GetOptions().map_entry:
                continue  # synthesized map entries are not decoded on their own
            checked += 1
            failures += check_message(harness, work, factory, descriptor, meta, messages, rng,
                                      seed_dir)
        return checked, None, failures


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--schema", type=Path, action="append",
                        help="a schema to check (repeatable; default: every tests/corpus/*.proto)")
    parser.add_argument("--messages", type=int, default=DEFAULT_MESSAGES,
                        help=f"random messages per message type (default: {DEFAULT_MESSAGES})")
    parser.add_argument("--seed", type=int, default=0, help="RNG seed (default: 0)")
    parser.add_argument("--build-dir", type=Path, default=DEFAULT_BUILD_DIR,
                        help="where rapidprotoc and rapidproto_diffgen were built")
    parser.add_argument("--cxx", default="g++", help="compiler for the generated harness")
    parser.add_argument("--write-seeds", type=Path, default=None, metavar="DIR",
                        help="also write every generated payload into DIR, as a fuzzer seed corpus")
    parser.add_argument("--verbose", action="store_true", help="name every schema and skip reason")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4,
                        help="schemas to check in parallel (default: CPU count)")
    args = parser.parse_args()

    try:
        import google.protobuf.message_factory  # noqa: F401  (presence check; workers re-import)
    except ImportError:
        print("differential: protobuf Python bindings not installed; skipping")
        return SKIP_RC

    # An explicit --build-dir that holds no tools is a typo, not a reason to report success; the
    # DEFAULT one being unbuilt is the legitimate skip (nobody must build to run an unrelated stage).
    tools = {"rapidprotoc": args.build_dir / "rapidprotoc",
             "diffgen": args.build_dir / "rapidproto_diffgen"}
    explicit_build_dir = args.build_dir != DEFAULT_BUILD_DIR
    for path in tools.values():
        if not path.is_file():
            if explicit_build_dir:
                raise SystemExit(f"--build-dir {args.build_dir}: {path.name} is not built there")
            print(f"differential: {path} not built; skipping")
            return SKIP_RC
    protoc = shutil.which("protoc")
    if protoc is None:
        print("differential: protoc not on PATH; skipping")
        return SKIP_RC
    tools["protoc"] = Path(protoc)

    seed_dir = args.write_seeds
    if seed_dir is not None:
        seed_dir.mkdir(parents=True, exist_ok=True)
        # Clear what a previous run left: payloads are named by message and index, so a re-run with
        # a smaller --messages would otherwise leave the higher-index files of the older, larger run
        # sitting in the seed corpus, unexplained and never regenerated.
        for stale in seed_dir.glob("*.bin"):
            stale.unlink()

    # rglob: the cross-file fixtures live in tests/corpus/imports and tests/corpus/nsedge, and those
    # are exactly the ones that exercise imported types.
    schemas = args.schema or sorted(CORPUS.rglob("*.proto"))
    for schema in schemas:
        if not schema.is_file():
            raise SystemExit(f"--schema {schema} does not exist")
    # One worker per schema. Each compiles a harness and runs it, so the work is dominated by
    # subprocesses rather than Python -- but the payload generation and field comparison ARE Python,
    # so processes (not threads) to escape the GIL. map() yields in input order, which keeps the
    # skip/failure lines identical run to run regardless of who finishes first.
    checked = 0
    skipped: list[str] = []
    failures: list[str] = []
    with concurrent.futures.ProcessPoolExecutor(max_workers=args.jobs) as pool_exec:
        results = pool_exec.map(check_schema, schemas, repeat(tools), repeat(args.cxx),
                                repeat(args.seed), repeat(args.messages), repeat(seed_dir))
        for schema, (count, skip, schema_failures) in zip(schemas, results):
            checked += count
            if skip is not None:
                skipped.append(skip)
            failures += schema_failures
            if args.verbose:
                print(f"  {schema.name}: done")

    # Skips are printed every run, not just under --verbose: they are how a schema silently leaves
    # coverage, and the count drifting is the thing worth noticing.
    for reason in skipped:
        print("   skipped " + reason)
    for failure in failures:
        print(">> " + failure)
    # Tool failures are counted apart from mismatches: one means our tools broke, the other
    # means a decode disagreed with protobuf, and reporting a tool failure as a "mismatch" sends
    # the reader looking for a decode bug that is not there. Build-level failures abort their
    # whole schema (build_schema raises), so they alone subtract from the schema count; a wire
    # mutator failure is per message type and leaves the rest of its schema checked.
    build_failures = sum(1 for f in failures if "rejects a protoc-valid schema" in f
                         or "diffgen failed" in f or "harness does not compile" in f)
    tool_failures = build_failures + sum(1 for f in failures if "wire mutator" in f)
    mismatches = len(failures) - tool_failures
    print("differential: %d message types over %d schemas, %d payloads each (plus wire-shuffled "
          "variants), %d mismatches "
          "(%d schemas skipped%s)" % (checked, len(schemas) - len(skipped) - build_failures,
                                      args.messages, mismatches, len(skipped),
                                      ", %d tool failure%s"
                                      % (tool_failures, "" if tool_failures == 1 else "s")
                                      if tool_failures else ""))
    if checked == 0:
        why = ("every schema hit a tool failure" if tool_failures else "every schema skipped")
        print(">> nothing was checked: %s. Something upstream is broken -- a green result here "
              "would mean only that the test ran, not that anything decoded." % why)
        return 1
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
