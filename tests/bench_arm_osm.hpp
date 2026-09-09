// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter
#pragma once

// The OSM PBF scenarios: decode a real OpenStreetMap extract (a PINNED Geofabrik snapshot,
// tests/fetch_osm_dataset.py) against libosmium, the de-facto standard C++ OSM library,
// compiled from the corpus pin. Two scenarios, because zlib is the elephant in any PBF
// benchmark -- the blobs must be inflated before protobuf work starts, and that step rivals
// the decode itself:
//
//   osm_blocks -- every PrimitiveBlock pre-inflated ONCE (untimed); arms decode+walk the
//     payloads. This isolates the protobuf layer: protoc / arena-warm / arena-cold /
//     streamgen. Reuse is granted symmetrically -- protoc keeps ONE message across blocks (its
//     standard repeated-parse idiom, and its fastest), arena-warm resets one arena, arena-cold
//     constructs per block. libosmium cannot join (its public API starts at the blob framing).
//   osm_file -- the raw file bytes in memory; arms do everything: framing, blob decode,
//     inflate, block decode, walk. arena-warm / streamgen / libosmium.
//
// Like the compute and messages arms this lives in its OWN translation unit: measured
// variants sharing a TU re-time each other on any edit.
//
// CHECKSUM CONVENTION: same work in every arm, validated before anything is timed. Per
// entity: a type-weighted id; the osmium-formula coordinates ((raw*granularity+offset)/100 as
// int32); metadata when present (version, timestamp in seconds via (raw*date_granularity)/
// 1000, changeset, uid, first byte of the username); the first byte of every tag KEY and tag
// VALUE (forces stringtable resolution without measuring strlen); each way ref, relation
// member id (both delta-RESOLVED to absolute), member role first-byte and member type. All
// sums wrap in uint64. The streaming arm consumes the DenseInfo/Info columns and all
// key/value/role/type index arrays through callbacks rather than skipping them, so no arm
// gets a skip-what-others-parse discount; its two passes per block (stringtable first) are
// timed as part of the arm -- that is the model's honest cost.
//
// Residual asymmetries, stated rather than hidden:
//   - libosmium always materializes full OSM objects (that IS its model -- entities are built
//     in its buffers, usernames copied in, before a handler sees them); the arena arm is the
//     like-for-like materializing comparison.
//   - Both rapidproto osm_file arms decode the tiny per-blob envelope (BlobHeader/Blob, 247
//     messages) through one shared arena-model helper -- equal cost for both, ~0 either way.
//     Both also parse-and-discard the OSMHeader block, which libosmium reads internally.
//   - The convention replicates osmium's conversions only as far as the pinned dataset
//     exercises them: a HISTORY or anonymous-edit dataset (uid < 0, visible flags, which
//     osmium clamps or handles specially) and files with plain non-dense Nodes or
//     uncompressed blobs are NOT covered by the cross-validation on this dataset -- swapping
//     the pin for such a file would fail validation loudly, not silently mismeasure.
//
// THREADING: the harness pins the whole process to one core, and run_arm() forces that pin
// BEFORE libosmium's lazy worker pool is born (the pool inherits the affinity of the thread
// that first touches it), so every arm gets exactly one core's worth of CPU regardless of
// scenario filtering. The numbers are single-core throughput, which is the honest basis for a
// decoder comparison; libosmium's multi-core scaling is real but is a different question. The
// libosmium arm is marked `threaded`, which makes the harness compare the osm_file scenario on
// WALL time (per-thread cycle counters see only the calling thread) and drop the arm's
// main-thread-only cyc/B / ins/B columns.

namespace rposm {

// Cross-validate every arm's checksum on the dataset, then measure the two scenarios.
// Returns the harness's per-arm mismatch count (0 = clean), or -1 when validation or the
// dataset load already failed. Compiled only under RAPIDPROTO_BENCH_OSM; the caller guards
// the call the same way.
int run_arm();

}  // namespace rposm
