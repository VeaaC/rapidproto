#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Christian Vetter
"""Generate testdata/mini.osm.pbf, the committed fixture the osmstat test decodes.

Hand-encoded protobuf wire format -- no protobuf library, no schema compilation -- so the
fixture regenerates byte-identically anywhere (check_fixture.py holds the committed file to
that). Deliberate coverage: a zlib blob AND a raw blob; dense nodes (with tags, interleaved
keys_vals, and DenseInfo) AND a plain node; a way and a relation; the default coordinate
granularity AND an explicit granularity with nonzero offsets (the proto2-default paths differ
between the two decode models -- see the mains).
"""
import struct
import zlib


def varint(n: int) -> bytes:
    out = bytearray()
    n &= (1 << 64) - 1
    while True:
        b = n & 0x7F
        n >>= 7
        out.append(b | (0x80 if n else 0))
        if not n:
            return bytes(out)


def zigzag(n: int) -> int:
    return (n << 1) ^ (n >> 63)


def tag(field: int, wire_type: int) -> bytes:
    return varint(field << 3 | wire_type)


def f_varint(field: int, n: int) -> bytes:
    return tag(field, 0) + varint(n)


def f_bytes(field: int, payload: bytes) -> bytes:
    return tag(field, 2) + varint(len(payload)) + payload


def f_str(field: int, s: str) -> bytes:
    return f_bytes(field, s.encode())


def packed(field: int, values, encode=varint) -> bytes:
    return f_bytes(field, b"".join(encode(v) for v in values))


def packed_sint(field: int, values) -> bytes:
    return packed(field, [zigzag(v) for v in values])


def deltas(values):
    prev = 0
    out = []
    for v in values:
        out.append(v - prev)
        prev = v
    return out


def header_block() -> bytes:
    # HeaderBBox: required sint64 left/right/top/bottom (nanodegrees).
    bbox = (tag(1, 0) + varint(zigzag(8_800_000_000))
            + tag(2, 0) + varint(zigzag(8_800_004_000))
            + tag(3, 0) + varint(zigzag(53_000_002_000))
            + tag(4, 0) + varint(zigzag(53_000_000_000)))
    return (f_bytes(1, bbox)
            + f_str(4, "OsmSchema-V0.6")
            + f_str(4, "DenseNodes")
            + f_str(16, "make_fixture.py"))


def dense_block() -> bytes:
    """Block 1 (zlib): stringtable + a dense-node group, a way, a relation. Default granularity."""
    strings = [b"", b"highway", b"residential", b"name", b"Fixture Way", b"amenity", b"cafe"]
    stringtable = b"".join(f_bytes(1, s) for s in strings)

    dense_info = (packed(1, [1, 1, 1])                          # version
                  + packed_sint(2, deltas([100, 200, 300]))     # timestamp
                  + packed_sint(3, deltas([7, 7, 8]))           # changeset
                  + packed_sint(4, deltas([42, 42, 42]))        # uid
                  + packed_sint(5, deltas([0, 0, 0])))          # user_sid
    dense = (packed_sint(1, deltas([1001, 1002, 1003]))
             + f_bytes(5, dense_info)
             + packed_sint(8, deltas([530_000_000, 530_000_010, 530_000_020]))  # lat
             + packed_sint(9, deltas([88_000_000, 88_000_010, 88_000_020]))     # lon
             # node 1001: highway=residential; node 1002: no tags; node 1003: amenity=cafe, name=...
             + packed(10, [1, 2, 0, 0, 5, 6, 3, 4, 0]))
    way = (f_varint(1, 2001)
           + packed(2, [1, 3]) + packed(3, [2, 4])              # highway=residential, name=...
           + f_bytes(4, f_varint(1, 2))                          # Info{version: 2}
           + packed_sint(8, deltas([1001, 1002, 1003])))         # refs
    relation = (f_varint(1, 3001)
                + packed(2, [5]) + packed(3, [6])                # amenity=cafe
                + packed(8, [0, 0])                              # roles_sid
                + packed_sint(9, deltas([1001, 2001]))           # memids
                + packed(10, [0, 1]))                            # types: NODE, WAY
    return (f_bytes(1, stringtable)
            + f_bytes(2, f_bytes(2, dense))                      # group A: dense
            + f_bytes(2, f_bytes(3, way))                        # group B: ways
            + f_bytes(2, f_bytes(4, relation)))                  # group C: relations


def plain_block() -> bytes:
    """Block 2 (raw): one plain (non-dense) tagged node under granularity 200 + offsets."""
    strings = [b"", b"amenity", b"cafe"]
    stringtable = b"".join(f_bytes(1, s) for s in strings)
    node = (tag(1, 0) + varint(zigzag(4001))
            + packed(2, [1]) + packed(3, [2])                    # amenity=cafe
            + f_bytes(4, f_varint(1, 1))                          # Info{version: 1}
            + tag(8, 0) + varint(zigzag(265_000_000))             # lat: 100 + 200*raw nanodeg
            + tag(9, 0) + varint(zigzag(44_000_000)))             # lon: -100 + 200*raw nanodeg
    return (f_bytes(1, stringtable)
            + f_bytes(2, f_bytes(1, node))                        # group: plain nodes
            + f_varint(17, 200)                                   # granularity
            + f_varint(19, 100)                                   # lat_offset
            + f_varint(20, -100))                                 # lon_offset (varint masks to 2's complement)


def blob_frame(blob_type: str, payload: bytes, compress: bool) -> bytes:
    if compress:
        blob = f_varint(2, len(payload)) + f_bytes(3, zlib.compress(payload, 9))
    else:
        blob = f_bytes(1, payload)
    header = f_str(1, blob_type) + f_varint(3, len(blob))
    return struct.pack(">I", len(header)) + header + blob


def build() -> bytes:
    return (blob_frame("OSMHeader", header_block(), compress=False)
            + blob_frame("OSMData", dense_block(), compress=True)
            + blob_frame("OSMData", plain_block(), compress=False))


if __name__ == "__main__":
    import sys
    path = sys.argv[1] if len(sys.argv) > 1 else "mini.osm.pbf"
    with open(path, "wb") as f:
        f.write(build())
    print(f"wrote {path}")
