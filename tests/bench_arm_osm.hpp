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
//     payloads. This isolates the protobuf layer: protoc / rapidproto-arena /
//     rapidproto-stream. libosmium cannot join (its public API starts at the blob framing).
//   osm_file -- the raw file bytes in memory; arms do everything: framing, blob decode,
//     inflate, block decode, walk. rapidproto-arena / rapidproto-stream / libosmium.
//
// Like the compute and messages arms this lives in its OWN translation unit: measured
// variants sharing a TU re-time each other on any edit.
//
// CHECKSUM CONVENTION: same work in every arm, validated before anything is timed. Per
// entity: a type-weighted id, the osmium-formula coordinates ((raw*granularity+offset)/100,
// int32), metadata when present (version, timestamp in seconds via (raw*date_granularity)/
// 1000, changeset, uid, first byte of the username), the first byte of every tag KEY (forces
// stringtable resolution without measuring strlen), each way ref and relation member id
// delta-RESOLVED (absolute). All sums wrap in uint64. Streaming consumes the DenseInfo/Info
// columns through callbacks rather than skipping them, precisely so no arm gets a
// skip-what-others-parse discount. Known residual asymmetry, stated rather than hidden:
// libosmium always materializes its OSM objects (that IS its model -- entities are built in
// its buffers before a handler sees them, usernames copied in), so its arm carries
// materialization work the streaming arm does not have; the arena arm is the like-for-like
// materializing comparison.
//
// THREADING: the harness pins the whole process to one core, so libosmium's reader threads
// (it decompresses and parses on a pool) all share that core -- every arm gets exactly one
// core's worth of CPU. The numbers are single-core throughput, which is the honest basis for
// a decoder comparison; libosmium's multi-core scaling is real but is a different question.
// The arm is marked `threaded`, which makes the harness compare the osm_file scenario on WALL
// time (per-thread cycle counters see only the calling thread) and drop the arm's
// main-thread-only cyc/B / ins/B columns.

namespace rposm {

// Cross-validate every arm's checksum on the dataset, then measure the two scenarios.
// Returns the harness's per-arm mismatch count (0 = clean), or -1 when validation or the
// dataset load already failed. Compiled only under RAPIDPROTO_BENCH_OSM; the caller guards
// the call the same way.
int run_arm();

}  // namespace rposm
