# Third-Party Notices

RapidProto is licensed under Apache-2.0 (see `LICENSE`). It includes or uses the
third-party software listed below. Components marked **distributed** ship inside
the library and/or its generated output and carry their own license terms;
components marked **development-only** are used to build/test/benchmark RapidProto
and are not part of what it distributes.

---

## Distributed

### Protocol Buffers — well-known-type definitions

- **Files:** `wellknown/*.proto`, embedded as string literals into
  `src/wellknown_generated.cpp` (so a schema can reference `google.protobuf.*`
  types and still generate standalone). The original BSD-3 header is preserved in
  each embedded `.proto`.
- **Copyright:** 2008 Google Inc. All rights reserved.
- **License:** BSD 3-Clause:

  > Redistribution and use in source and binary forms, with or without
  > modification, are permitted provided that the following conditions are met:
  >
  >   * Redistributions of source code must retain the above copyright notice,
  >     this list of conditions and the following disclaimer.
  >   * Redistributions in binary form must reproduce the above copyright notice,
  >     this list of conditions and the following disclaimer in the documentation
  >     and/or other materials provided with the distribution.
  >   * Neither the name of Google Inc. nor the names of its contributors may be
  >     used to endorse or promote products derived from this software without
  >     specific prior written permission.
  >
  > THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
  > AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
  > IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
  > ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
  > LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
  > CONSEQUENTIAL DAMAGES ... ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE.

---

## Development-only (not distributed)

### Catch2

- **Files:** `tests/catch_amalgamated.{hpp,cpp}` (vendored unit-test framework).
- **License:** Boost Software License 1.0 (BSL-1.0).

### protozero

- **Use:** referenced only by `tests/bench_streamgen.cpp` (the decode benchmark),
  via a system/dev install. It is not linked into the library or its output.
- **Copyright:** Mapbox.
- **License:** BSD 2-Clause.
