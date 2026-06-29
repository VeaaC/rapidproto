# Security Policy

## Threat model

RapidProto is a **decode-only** Protobuf library. Its security boundary is the **serialized message
bytes** passed to a generated decoder: those bytes are treated as **untrusted**. The schema
(`.proto`) is assumed to be trusted: you compile your own schemas, having already passed `protoc`.

The core guarantee: a generated decoder (arena or streaming) and the runtime **must never crash,
read or write out of bounds, recurse without bound, or otherwise exhibit undefined behavior on *any*
input bytes**, whether well-formed or malformed. Malformed input fails cleanly (a wire error), never
unsafely. The wire reader is fully validating: varint overflow, truncation, length overruns,
reserved wire types, and group nesting are all detected and depth-capped.

## In scope

- A crash, hang, out-of-bounds access, unbounded memory or recursion, or any other undefined
  behavior triggered by feeding untrusted bytes to a generated decoder or the runtime.
- A way to make a decoder exceed its documented limits (e.g. the nesting-depth cap) unsafely.

## Out of scope

- **Incorrect decoded values** for input that was not produced by a conformant Protobuf encoder.
  RapidProto validates wire *structure*, not field *value* semantics; field values are not
  range-checked (that is the schema's job, which the library trusts). Out-of-range enum values and
  the like decode without error by design.
- Issues that require a **malicious `.proto` schema**; schemas are trusted input.
- The benchmarks, tests, and examples.

## How this is tested

Memory safety on untrusted input is exercised continuously: libFuzzer harnesses over the wire reader
and both decode models, run under AddressSanitizer + UndefinedBehaviorSanitizer (`./check.sh deep`,
which CI runs on every pull request).

## Reporting a vulnerability

Please report security issues **privately** via GitHub's [private vulnerability
reporting](https://docs.github.com/en/code-security/security-advisories/guidance-on-reporting-and-writing-information-about-vulnerabilities/privately-reporting-a-security-vulnerability):
the repository's **Security** tab → **Report a vulnerability**. Please do **not** open a public
issue for a suspected vulnerability.

This is an early-stage (0.x) project maintained on a best-effort basis; reports will be acknowledged
as soon as possible. Only the latest released version is supported.
