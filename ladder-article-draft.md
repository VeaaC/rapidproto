# A protobuf decoder, one optimization at a time

RapidProto is a code generator that turns protobuf schemas into C++ decoders - an arena
decoder that materializes a whole message tree, and a streaming decoder that hands each field
to a callback. Both ended up fast, but "fast" arrived one change at a time, and some of these
optimizations are quite interesting. This article tries to go through a few of them, explain
them, and show the impact they have.

## How these numbers were made

What is measured is the generated decoder's throughput divided by a reference decoding the
same bytes in the same binary: protoc (libprotobuf 4.25.3) for arena benchmarks, protozero
for streaming benchmarks. A step's change on a benchmark is shown in the charts only when it
exceeds that benchmark's own measured noise.

One caveat: everything here was produced at one point in time; the maintained numbers are in
[benchmarks.md](benchmarks.md).

## The baseline

The starting point is the decoder a careful first implementation would produce. One loop per
message: read a tag, switch on the field number, decode the value into the struct, validate
everything on the way. Repeated fields append to a geometrically growing array. Varint
readers have a 1-byte fast path. Strings are copied. Every sub-message lives behind a
pointer. The compiler inlines whatever it feels like.

With all of that, the arena baseline decodes `Dataset` at 2.4× protoc - and the streaming
baseline sits at 0.9× protozero.

![Baseline](ladder-step0.svg)

## Step 1: borrowed strings

A decoded string does not need its own bytes - the input buffer already has them. The arena
stores every string and bytes field as a {pointer, length} view into the input: no copy, no
allocation, no small-string optimization to branch on.

`Dataset` is string-heavy and gains ×1.42; `google_message2` gains ×1.14. Arena memory for
`Dataset` drops from 1.23 MB to 1.03 MB - against protoc's 1.97 MB for the same payload.
The streaming side is unaffected by construction: its API has handed out `string_view`s from
the beginning.

![Step 1](ladder-step1.svg)

## Step 2: packed pre-sizing

A packed repeated field arrives length-prefixed, and the length says a lot. For fixed-width
elements it gives the exact element count; for varints it bounds the count from above (each
element is at least one byte). So instead of growing an array element by element, the
decoder allocates the bound once, decodes into place, and returns the unused tail to the
arena - the trim is a pointer subtraction, since nothing else allocated in between.

For fixed-width elements on a little-endian machine there is a second consequence: the wire
span already is the array's byte image, so the whole fill is one `memcpy`. That is how
`packed double` gains ×1.93. The varint sweeps gain ×1.3-1.4 from the pre-sizing alone, and
`few msgs, big arrays` gains ×1.57. Twenty benchmarks speed up on this step, none slow
down. The streaming decoder passes each value to a callback, so there is no array to
pre-size and it stays where it was.

![Step 2](ladder-step2.svg)

## Step 3: forced inlining

The decode loop calls small wire primitives constantly - read a varint, read a length, skip
a value. `RP_FLATTEN` (gcc and clang's `flatten` attribute) tells the compiler to inline
the entire call tree into each generated decode function, and it turns out compilers are
far too conservative, clang included: without the attribute, per-element read helpers stay
out of line and the cursor round-trips through calls.

This is the broadest optimization step - 37 benchmarks improve. The 1M varint sweep gains
×3.04, the streaming version of the same sweep gains ×2.25 (crossing protozero here for the
first time), and `sparse-skip` gains ×1.71 because the skip helpers now inline too.

Two costs. First, unbounded flattening would inline entire sub-message closures into their
parents, so the layout planner marks large decoders as inline barriers - flatten inside,
never absorbed. Second, one regression: the nesting-heavy streaming `nested-msg` benchmark
drops ×0.68 on this step and never recovers; flattening a recursion-heavy shape can make it
worse. Compile time and code size also pay for this step; the shipped build's compile costs
are tracked in [benchmarks.md](benchmarks.md).

![Step 3](ladder-step3.svg)

## Step 4: fused tag reads

Every field starts with the same three questions: is the buffer done, is the next varint a
valid tag, what field is it. Asking them separately means two bounds checks per field. The
fused read answers all three from one bounds check, returning end, tag, or error in a single
step - and inside it, a tag byte below 128 short-circuits: field number and wire type fall
out of one byte with two shifts.

The gains concentrate where fields are small and plentiful: `Dataset` +11%,
`google_message1` +10%, `many msgs, tiny arrays` +11%. Three kernel-heavy sweeps read a few
percent slower, at the edge of their noise - small enough that this campaign cannot say
whether it is real.

![Step 4](ladder-step4.svg)

## Step 5: SWAR varint kernels

A packed repeated varint is decoded one byte at a time in the builds so far: read a byte,
test the continuation bit, and continue if necessary. This is inherently sequential, and on
mixed-width data branch prediction cannot help much - branch misprediction actually hurts
performance a lot.

One idea is to have dedicated decoders (so called kernels) for various value distributions.
A kernel loads 64 bits at a time and tries to decode multiple varints in parallel,
optimizing for a specific distribution. The high bit of each byte marks continuation, a
multiply-and-shift gathers those eight bits into a mask, and three shift-and-mask rounds
compact each value's 7-bit groups into a contiguous result. One branch for the whole 64-bit
word now replaces one branch per byte.

This requires, of course, that the decoder properly predicts the distribution: it probes the
first 64 bytes of each span and picks the kernel to match. As this adds overhead it only
makes sense for larger payloads (in our case 256+ bytes).

The kernels are written in a portable fashion - no SIMD intrinsics, just basic integer
instructions, a technique called SWAR - but often compile into SIMD instructions anyway.

The chart shows the branch-prediction story directly. The all-1-byte sweep (`rv fx1`) gains
13% - its continuation bit never varies, so the branch it lost was a predicted one. The
mixed-width datasets gain the most: ×2.33 on the enum mix, ×1.29 on uniformly random
widths. The streaming side stays unaffected: that decoder hands each value to a callback as
it is decoded, and without a bulk fill the kernels remain unused.

![Step 5](ladder-step5.svg)

## Step 6: the one-byte peek hub

Fields numbered 1 through 15 have single-byte tags, and most messages live entirely in that
range. So before doing a full tag read, the loop peeks at the raw byte and switches on it
directly - each known tag byte jumps straight to a label that decodes that field, tag
already consumed. Anything else (higher fields, unknown fields, a non-minimal encoding)
falls through to the general path from step 4, unchanged.

On its own this step barely registers: a single benchmark moves, `Dataset` at ×1.23. A
switch on a byte is still a switch. The hub's real job is structural - it turns each field's
decode into an addressable label, and labels are what the next step needs.

![Step 6](ladder-step6.svg)

## Step 7: field-order threading

Encoders write fields in ascending field-number order - protoc does, and the spec recommends
it. That makes the next tag predictable: after field 3, expect field 4, or 5. So each
field's label ends by comparing the next byte against the constant tags of the next expected
fields, and jumping straight to their labels on a match. A repeated field checks for another
element of itself first. A oneof's siblings are skipped - at most one member occurs. On an
in-order wire the decoder becomes a chain of direct, correctly predicted branches; the
dispatch switch from step 6 only catches the exceptions.

The dispatch-bound benchmark gains ×1.56, `google_message1` ×1.23, and `Dataset` finishes at
6.1× protoc. Two benchmarks lose ground: `osm_blocks` −4% and `many msgs, tiny arrays` −10%
- wire whose field order defeats the prediction pays for the failed probes.

This is the shipped decoder.

![Step 7](ladder-step7.svg)

## What is not covered

Value-threading - passing the wire cursor by value through free functions instead of a
reader object - predates this comparison and is baked into the baseline; un-building it
would have meant resurrecting a deleted design. Its measurement at the time: −17%
instructions on gcc's arena decoder, −15% to −41% on streaming. The arena's chunk sizing
and the growable-array strategy were tuned by comparing designs rather than by switching
one off, and those comparisons live with the code. The struct-layout planner (member
reordering, inlining small sub-messages) is also absent: measured on this suite it moves
nothing beyond noise.

The suite, the references, the wire payloads, and the measurement scripts are all in the
[repository](https://github.com/VeaaC/rapidproto); the maintained numbers are in
[benchmarks.md](benchmarks.md). Campaign: one revision, clang 20, September 2026.
