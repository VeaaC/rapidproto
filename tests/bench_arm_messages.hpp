// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter
#pragma once

// The google_message1/google_message2 scenarios (roadmap 3.5): protobuf's own published
// cross-language benchmark payloads -- anonymized real production message shapes -- so numbers
// measured here are checkable against figures third parties already know. Both come from the
// pinned protobuf-benchmarks corpus source (tests/fetch_corpus.py); the scenarios are absent
// (and say so) without it. Like the compute arm, this lives in its OWN translation unit:
// measured variants sharing a TU re-time each other on any edit (see bench_arm_compute.hpp).
//
// The wire is the dataset file's `payload[0]` (a BenchmarkDataset wrapper: field 3, repeated
// bytes; both files carry exactly one payload), unwrapped by hand below -- the wrapper schema
// is not worth a generated decoder.
//
// CHECKSUM CONVENTION: present fields only, every field of the schema walked. Proto2 defaults
// are non-zero (field129's default is a 21-char string), and the streaming walk is wire-driven
// -- it cannot see an absent field's default -- so reading defaults would disagree across
// arms. upb implements the rule twice -- a generic reflection walk (validation only) and the
// hand-written accessor walks its timed arms run -- and startup pins every walk to one
// checksum before anything is measured.
//
// google_message1 is 228 bytes decoded repeatedly (the official suite uses it the same way);
// google_message2 is ~84.5 KB, 98% of it one repeated GROUP -- the shape protoc's proto2
// heritage is tuned for.

// Declaration only: the generated headers this arm decodes with live in
// its own translation unit (bench_arm_messages.cpp), for the same placement reason as
// bench_arm_compute.hpp, and because a TU must include ONE generated tree's runtime copy.

namespace rpmessages {

// Cross-validate every decoder's checksum on both payloads, then measure the two scenarios.
// Returns the harness's per-arm mismatch count (0 = clean), or -1 when validation or the
// dataset load already failed. Compiled only under RAPIDPROTO_BENCH_MESSAGES; the caller
// guards the call the same way.
int run_arm();

}  // namespace rpmessages
