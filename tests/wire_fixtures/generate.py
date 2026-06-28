#!/usr/bin/env python3
"""Encode the wire-format test fixtures with protoc.

For each (schema, message, textproto input) below, runs
    protoc --encode=<message> -I<dirs> <schema> < <input>.txtpb > <name>.bin
and writes the binary fixture next to this script. The .bin files are checked in; the
tests read them directly and never invoke protoc. protoc is needed ONLY to regenerate
fixtures (a dev/CI dependency), so this script is the only thing that depends on it.

Run from anywhere:
    python3 tests/wire_fixtures/generate.py
"""

import pathlib
import shutil
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
CORPUS = HERE.parent / "corpus"

# name -> (schema path, message, include dirs, textproto input)
FIXTURES = {
    "scalars": (CORPUS / "proto2.proto", "p2.Scalars", [CORPUS], HERE / "scalars.txtpb"),
    "msg": (CORPUS / "proto3.proto", "p3.Msg", [CORPUS], HERE / "msg.txtpb"),
    "container": (CORPUS / "proto2.proto", "p2.Container", [CORPUS], HERE / "container.txtpb"),
    "all_wire": (HERE / "wire_all.proto", "wire.AllWire", [HERE], HERE / "all_wire.txtpb"),
}


def main() -> int:
    protoc = shutil.which("protoc")
    if protoc is None:
        print(
            "protoc not found on PATH. It is required ONLY to regenerate the wire "
            "fixtures; the checked-in .bin files let the tests run without it.",
            file=sys.stderr,
        )
        return 1

    for name, (schema, message, includes, txtpb) in FIXTURES.items():
        out = HERE / f"{name}.bin"
        cmd = [protoc, f"--encode={message}"]
        for inc in includes:
            cmd.append(f"-I{inc}")
        cmd.append(str(schema))
        with txtpb.open("rb") as stdin, out.open("wb") as stdout:
            result = subprocess.run(cmd, stdin=stdin, stdout=stdout, check=False)
        if result.returncode != 0:
            print(f"protoc failed for {name} ({message})", file=sys.stderr)
            return result.returncode
        print(f"wrote {out.relative_to(HERE.parent.parent)} ({out.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
