#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Christian Vetter
"""Emit a text-format `google.cloud.compute.v1beta.Instance` for the arena benchmark.

Why this exists: the arena bench's other scenarios decode a schema we wrote (tests/bench/
bench.proto), which cannot show what a LARGE real-world schema costs. compute.proto is the
largest real schema in the fetched corpus -- 103k lines, 2223 generated decoders, and a single
`Instance` decoder whose object is 646,334 bytes of `.text` against 163,864 when built with
`-DRP_FLATTEN=` (3.9x), taking 99s to compile against 9s. It therefore exercises the question of
whether that code growth costs THROUGHPUT and not only build time -- a question this benchmark
narrows but does not settle; see the flatten knob in architecture.md.

Unlike descriptor.proto, compute.proto has no naturally-occurring payload, so this synthesizes a
plausible one: an instance with many attached disks, network interfaces, labels, service
accounts, accelerators and a metadata block. The field values are invented; the shape is the
schema's own. protoc turns this into wire bytes at build
time (`protoc --encode`), so nothing derived is committed and a corpus pin bump cannot leave a
stale payload behind.

The payload is deliberately modest (~40 KB). The hypothesis under test is about the size of the
DECODER, not of the input: a big payload through a small decoder would measure the opposite of
what we want.

It populates as much of `Instance`'s sub-message closure as it reasonably can, and that breadth
is the point rather than decoration. An earlier version set only `disks`, `network_interfaces`
and `labels`, which left **20 of the 23 sub-decoders in the closure never executed** -- and code
that never runs costs no instruction cache, so the arm could not test whether the flattened
decoder's SIZE costs throughput. If you trim this, you narrow what the benchmark can see.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

DISKS = 200
INTERFACES = 100
LABELS = 300


def textproto() -> str:
    out = [
        'name: "bench-instance"',
        'description: "synthetic instance for decoder benchmarking"',
        'machine_type: "zones/europe-west1-b/machineTypes/n2-standard-8"',
        'status: "RUNNING"',
    ]
    # Repeated sub-messages dominate: they exercise the arena's array growth and the
    # sub-message decode path, which is exactly what flatten inlines.
    for i in range(DISKS):
        out.append(f'disks {{ device_name: "disk-{i}" disk_size_gb: {100 + i} '
                   f'boot: {"true" if i == 0 else "false"} type: "PERSISTENT" '
                   f'mode: "READ_WRITE" interface: "SCSI" }}')
    for i in range(INTERFACES):
        out.append(f'network_interfaces {{ name: "nic-{i}" '
                   f'network: "projects/bench/global/networks/net-{i}" '
                   f'network_i_p: "10.{i // 256}.{i % 256}.2" stack_type: "IPV4_ONLY" }}')
    # A map field: its entries decode through the map path rather than the array path.
    for i in range(LABELS):
        out.append(f'labels {{ key: "label-{i}" value: "value-{i}" }}')
    # Reach further into the closure: each of these enters a sub-decoder the three fields above
    # never touch, so the measured code is a real fraction of what the schema generates rather
    # than three leaves of it.
    for i in range(40):
        out.append(f'service_accounts {{ email: "svc-{i}@bench.iam" '
                   f'scopes: "https://www.googleapis.com/auth/devstorage.read_only" '
                   f'scopes: "https://www.googleapis.com/auth/logging.write" }}')
        out.append(f'guest_accelerators {{ accelerator_type: "nvidia-tesla-t4" '
                   f'accelerator_count: {i % 8} }}')
    out += [
        'scheduling { automatic_restart: true on_host_maintenance: "MIGRATE" preemptible: false }',
        'shielded_instance_config { enable_secure_boot: true enable_vtpm: true '
        'enable_integrity_monitoring: true }',
        'shielded_instance_integrity_policy { update_auto_learn_policy: true }',
        'confidential_instance_config { enable_confidential_compute: false }',
        'advanced_machine_features { enable_nested_virtualization: false threads_per_core: 2 }',
        'display_device { enable_display: false }',
        'reservation_affinity { consume_reservation_type: "ANY_RESERVATION" }',
        'network_performance_config { total_egress_bandwidth_tier: "DEFAULT" }',
        'tags { items: "http-server" items: "https-server" fingerprint: "abc123" }',
    ]
    # `metadata` is a SINGULAR Metadata message whose `items` is the repeated part -- one block,
    # many items, not many blocks.
    items = " ".join(f'items {{ key: "meta-{i}" value: "mvalue-{i}" }}' for i in range(60))
    out.append(f'metadata {{ fingerprint: "md-fp" {items} }}')
    return "\n".join(out) + "\n"


# protoc warns that compute.proto does not use one of its own imports. That is a property of the
# fetched corpus, not of anything here -- but the quality gate greps build output for "warning:",
# so an unfiltered pass-through turns a third-party schema's lint into a red build.
BENIGN = re.compile(r"warning: Import .* is unused\.$")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--protoc", required=True)
    parser.add_argument("--proto", required=True, help="compute.proto")
    parser.add_argument("-I", dest="includes", action="append", default=[])
    parser.add_argument("--message", default="google.cloud.compute.v1beta.Instance")
    parser.add_argument("--out", required=True, help="wire-format payload to write")
    args = parser.parse_args()

    if not shutil.which(args.protoc) and not os.path.isfile(args.protoc):
        raise SystemExit(f"protoc not found: {args.protoc}")
    argv = [args.protoc, *[f"-I{i}" for i in args.includes],
            f"--encode={args.message}", args.proto]
    # Binary on stdout: text mode would corrupt the wire bytes. stderr is decoded separately.
    proc = subprocess.run(argv, input=textproto().encode(), capture_output=True)
    stderr = proc.stderr.decode("utf-8", "replace")
    noise = [l for l in stderr.splitlines() if l.strip() and not BENIGN.search(l)]
    if proc.returncode != 0:
        sys.stderr.write("\n".join(noise) + "\n")
        return proc.returncode
    for line in noise:  # anything protoc said that was not the known-benign import lint
        sys.stderr.write(line + "\n")

    # Write via a temp file and rename: a shell `>` redirect leaves a ZERO-BYTE payload behind
    # when the encode fails, and the next build considers that up to date.
    out = os.path.abspath(args.out)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    if not proc.stdout:
        sys.stderr.write("protoc --encode produced no output\n")
        return 1
    fd, tmp = tempfile.mkstemp(dir=os.path.dirname(out), suffix=".part")
    try:
        with os.fdopen(fd, "wb") as handle:
            handle.write(proc.stdout)
        os.chmod(tmp, 0o644 & ~_umask())  # mkstemp forces 0600; honour the user's umask
        os.replace(tmp, out)
    except BaseException:
        os.unlink(tmp)
        raise
    return 0


def _umask() -> int:
    mask = os.umask(0)
    os.umask(mask)
    return mask


if __name__ == "__main__":
    sys.exit(main())
