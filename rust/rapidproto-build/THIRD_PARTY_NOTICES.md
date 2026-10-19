# Third-Party Notices — `rapidproto-build`

`rapidproto-build` is licensed under Apache-2.0 (see `LICENSE` and `NOTICE`). It
distributes one prebuilt binary that statically links third-party code, whose
copyright and license notices are reproduced below.

---

## The code generator blob — `wasm/rustgen.wasm`

`wasm/rustgen.wasm` is RapidProto's own Rust code-generator front-end (first-party,
Apache-2.0) compiled to a `wasm32-wasi` command module. The crate `include_bytes!`s
it and runs it through the `wasmi` interpreter, so a consumer generates decoders
with cargo alone — no C++ toolchain.

**Provenance.** The blob is built by `wasm/build-wasm.sh` with a pinned
[wasi-sdk](https://github.com/WebAssembly/wasi-sdk) **34.0** (`wasi-libc`
`2e6fb9d8ee0c`, `llvm` `895aa2c896ad`). The build is byte-reproducible — same
inputs, same bytes — and `wasm/check-wasm.sh` is a CI gate that rebuilds the blob
from the current generator source and byte-diffs it against the committed copy, so
the two can never silently drift. To reproduce or audit the blob, install that
wasi-sdk and run `wasm/build-wasm.sh`.

Linking against the wasi-sdk sysroot statically embeds portions of the components
below into the blob.

### LLVM C++ runtime — libc++, libc++abi, compiler-rt builtins

- **Copyright:** the LLVM Project contributors.
- **License:** Apache License 2.0 **with LLVM exception** (see the Apache-2.0 text
  in `LICENSE`, plus the LLVM exception reproduced under "License texts" below).

### wasi-libc

- **Source:** <https://github.com/WebAssembly/wasi-libc> (as shipped in wasi-sdk 34.0).
- **License:** wasi-libc as a whole is multi-licensed under the Apache License v2.0
  with LLVM Exceptions, the Apache License v2.0, and the MIT License. It bundles the
  components below; each carries its own notice upstream:

  | Component | Copyright | License |
  |-----------|-----------|---------|
  | musl (`libc-top-half/musl`) | © 2005–2020 Rich Felker, et al. | MIT |
  | emmalloc | © the Emscripten authors | MIT |
  | dlmalloc | Doug Lea | CC0 1.0 (public-domain dedication) |
  | cloudlibc (`libc-bottom-half/cloudlibc`) | © 2015 Nuxi (https://nuxi.nl/) | BSD-2-Clause |
  | musl-fts (`fts`) | © the musl-fts contributors | BSD-3-Clause |

---

## License texts

### LLVM exception to the Apache 2.0 License

```
As an exception, if, as a result of your compiling your source code, portions
of this Software are embedded into an Object form of such source code, you
may redistribute such embedded portions in such Object form without complying
with the conditions of Sections 4(a), 4(b) and 4(d) of the License.

In addition, if you combine or link compiled forms of this Software with
software that is licensed under the GPLv2 ("Combined Software") and if a
court of competent jurisdiction determines that the patent provision (Section
3), the indemnity provision (Section 9) or other Section of the License
conflicts with the conditions of the GPLv2, you may retroactively and
prospectively choose to deem waived or otherwise exclude such Section(s) of
the License, but only in their entirety and only with respect to the Combined
Software.
```

### MIT License (musl, emmalloc, and wasi-libc's MIT option)

```
Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### BSD-2-Clause (cloudlibc)

```
Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES ... ARE DISCLAIMED. IN NO EVENT SHALL THE
COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, ... DAMAGES
... ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE.
```

### BSD-3-Clause (musl-fts)

Same terms as BSD-2-Clause above, plus: neither the names of the copyright
holders nor the names of their contributors may be used to endorse or promote
products derived from this software without specific prior written permission.

### Apache-2.0 with LLVM exception / CC0

The Apache License 2.0 text is in `LICENSE`. CC0 1.0 is a public-domain
dedication (no conditions). The full upstream notices for every component above
are preserved in the wasi-sdk and wasi-libc source trees named in the table.
