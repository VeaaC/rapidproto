# One optimization at a time

RapidProto can generate two C++ decoders for a schema. The [arena decoder](arena.md) decodes the
whole message into an arena and gives you an accessor per field, and the
[streaming decoder](streaming.md) hands the fields you ask for to your handlers and builds no
message. Most of their speed comes from seven optimizations. To measure each one, we switched all
seven off in the shipped code, along with two choices of the layout planner. What was left was a
plain loop that read a tag, switched on the field number and validated each value as it went. Then
we put the optimizations back one at a time. With all seven back, the arena decoder went from 2.4 to
6.1 times the throughput of [protoc](https://github.com/protocolbuffers/protobuf)'s generated code
on [`Dataset`](benchmarks.md#c), whose 2,000 proto3 records mix numbers, strings and sub-messages.

We measured the series in September 2026 with clang++-20 and protoc from libprotobuf 4.25.3;
[Benchmarks](benchmarks.md) has today's figures, against a newer protobuf release. Each build added
one optimization, in the order of the sections below. An extra build after borrowed strings turned
the two layout choices back on: members ordered by alignment to cut padding, and fixed-size
sub-messages of up to 16 bytes stored inside their parent. It moved no benchmark beyond its noise,
though the totals include it.

## The baseline

The baseline loop checked for the end of the input, read the next tag, switched on the field number,
checked the wire type and decoded the value into the message's struct. A repeated field went into an
array that started with room for four values and doubled when full, leaving each outgrown array in
the arena. Strings and bytes were copied into the arena. Clang inlined whatever it chose to.
Every build in the series, this one included, validated as much as the shipped decoder does.

It was already well ahead of protoc on `Dataset`, but protoc beat it on most packed varint arrays.
The streaming decoder was 11% slower than [protozero](https://github.com/mapbox/protozero) on
`rv fx1`, where every array in the [pool](benchmark-method.md#payloads) holds a million one-byte
varints.

![Chart of each benchmark's throughput relative to its reference decoder in the baseline, the
arena decoder against protoc on the left and the streaming decoder against protozero on the
right](ladder/ladder-step0.svg)

Every chart shows the same 43 benchmarks, the arena decoder's 23 on the left and the streaming
decoder's 20 on the right. The text quotes only a few of them. A bar is a decoder's throughput as a
multiple of its reference decoder's, on a square-root scale. The reference is protoc for the arena
decoder and protozero for the streaming decoder, run on the same bytes in the same program. Each
throughput is the median of five runs. In the step charts, numbered in the order of the sections
below, the part of a bar that changed since the build before is blue for a gain and orange for a
loss. It stays grey when the change is within the larger of 3% and the reference decoder's own
drift across the builds. protoc and protozero ran the same code in every build, but their speed
still moved with where the linker placed it ([code placement](benchmark-method.md#code-placement)).

## Borrowed strings

Each copied string took an arena allocation and a `memcpy`. `Dataset` has strings in every record.

A string or bytes field is a 12-byte `ArenaString`, a pointer and a 32-bit length, and its accessor
returns a `std::string_view` over them. In the baseline the pointer led to the arena's copy. Now it
points into the input.

![Diagram of the string "/index.html" in a message's path field. In the baseline it is copied from
the input into the arena with an allocation and a memcpy. With borrowed strings the field holds a
pointer and a length into the input.](ladder/diagram-strings.svg)

`Dataset` got 42% faster and used 1.03 MB of arena instead of 1.23 MB. The streaming decoder
didn't move, since its handlers had been given `std::string_view`s into the input all along.

![Chart of each benchmark's throughput relative to its reference decoder after step 1, borrowed
strings, with this step's change in color](ladder/ladder-step1.svg)

Borrowing slowed nothing down. It does tie the message to the input, which has to stay
[alive](arena.md#the-arena) while the message is in use. Where that's awkward,
[`decode_owned`](arena.md#owned-messages) keeps the input, the arena and the message together.

## Pre-sizing packed arrays

A packed field arrives as one length-delimited run of values. The baseline appended them one by one
to the growable array, with a capacity check per value and a copy at every doubling. It read
fixed-width values one at a time.

The run's length bounds how many values it holds. For fixed-width values the count is exact, the
length divided by the width. A varint takes at least one byte, so a run of varints holds at most
one value per byte. Pre-sizing allocates the array once at that size and decodes into it. For
varints it then hands the unused tail back with `Arena::shrink_last`, which moves the arena's
allocation pointer back to the end of the last value. Only the arena's most recent allocation can
shrink that way. Because decoding the run allocates nothing else, a freshly allocated array is still
the most recent one when the trim comes. On a little-endian host a run of fixed-width values is
already the array's memory image, and one `memcpy` fills it, as in protoc's decoder.

```cpp
// A sketch of the packed fill for a repeated int32 field, with plain names and no error checks,
// as of this step (the kernels for long runs come later). `values` and `count` are the array so
// far, `capacity` its room, `run` the packed bytes.
const std::size_t most = run.size();  // a varint takes at least one byte
if (capacity < count + most) {
    // allocate room for count + most values at once, copy the values from earlier runs
}
count += decode_varints(run, values + count);  // no capacity check per value
// give the unused tail back, if the array is still the arena's last allocation
arena.shrink_last(values, capacity * sizeof(std::int32_t), count * sizeof(std::int32_t));
capacity = count;  // trimmed or not, the room left is given up; a later run grows the array
```

![Diagram of a packed field of 87 varints in 190 bytes. The baseline grows its array to 4, 8, 16,
32 values and on, copying everything at each doubling. Pre-sizing allocates 190 values once, decodes
the 87 into place and returns the unused tail to the arena.](ladder/diagram-presize.svg)

On `packed double(fixed)`, where each of 200 messages holds 1,000 packed `double` values, the gain
was 93%. That took it from about half protoc's throughput to slightly more, now that both decoders
fill such an array with one copy. The `rv` pools of packed varints, from one to ten bytes per value,
gained 14% to 40% while still reading each varint a byte at a time.

![Chart of each benchmark's throughput relative to its reference decoder after step 2, packed
pre-sizing, with this step's change in color](ladder/ladder-step2.svg)

Pre-sizing cost no speed anywhere, but it raises the arena's peak while a packed varint field
decodes. Until the trim, the array has room for one value per byte of the run. For 64-bit values
that's 8 bytes of arena per input byte. A [capacity limit](arena.md#limiting-memory) has to leave
room for it.

## Forced inlining

The decode loop reads every value through small functions in `rapidproto::wire`, such as
`read_varint`, `read_length_delimited` and `skip_value`. Until this step we left it to clang to
decide which of those calls to inline.

Forced inlining puts `RP_FLATTEN` on every generated decode function and on the packed loops.

```cpp
#define RP_FLATTEN __attribute__((flatten))  // with gcc and clang; empty elsewhere
```

The `flatten` attribute inlines every call in a function's body, and every call inside those, all
the way down. It works in one direction only. It pulls callees into the function that carries it
and has no say in whether that function is inlined into its own callers.

Left alone, flatten would copy sub-message decoders into their parents all the way down the schema.
The arena generator limits this with a
[flatten budget](https://github.com/VeaaC/rapidproto/blob/main/architecture.md#tuning-benchmark-driven-knobs)
counted in decode arms, one per field or oneof member. For each message it adds up its own arms and
those of the sub-message decoders flatten would pull in. If that total is over four, and at least
one decoder is pulled in, the message also gets `RP_NOINLINE`. Take a message with three fields,
one of them a sub-message with two fields of its own. That makes five arms, one over the budget,
and the message is marked. It still inlines its sub-message, but a parent's flatten stops at it.

The arena decoder ran `rv fx1` 3.0 times as fast, with instructions per byte down from 25.75 to
11.75, and `Dataset` 22% faster. The streaming decoder overtook protozero on `rv fx1` and gained 71%
on `sparse-skip`. That message's fields come in groups of four, an `int32` with a handler and then
three without one, which the decoder skips: a varint, a `fixed64` and a length-delimited field.

![Chart of each benchmark's throughput relative to its reference decoder after step 3, forced
inlining, with this step's change in color](ladder/ladder-step3.svg)

The streaming decoder's `nested-msg` went the other way. It is one message holding 2,000,000
sub-messages of a single `int32`, one level deep, each decoded by a handler that calls the
sub-message's decoder. It ran 32% slower with a third more instructions per byte, and stayed there
for the rest of the series. We didn't track down why.

The build pays as well. Every inlined callee becomes a copy inside each decode function. Code size
and compile time grow with the schema. With g++-13, one googleapis schema of 288 messages
didn't finish compiling within 280 seconds with flatten unbounded and took 129 seconds with the
budget.

## Fused tag reads

Before a field can be decoded, the loop has to know whether any input is left, whether the next tag
is valid and what field it names. The baseline asked these one at a time, and checked for the end
of the input twice.

In the [encoding](https://protobuf.dev/programming-guides/encoding/#structure), a tag is a varint
holding the field number shifted left by three bits, with the wire type in the low three. For fields
1 to 15 it's a single byte below 0x80. The fused tag read, `read_tag_or_end`, answers all three
questions behind one bounds check and reports the outcome as a `TagState`. Fields 16 to 2047 get a
two-byte path of their own, and a longer or invalid tag goes on to the full `read_tag`.

```cpp
// A sketch of the loop's top, before and after, with error reporting left out.
// Before: the loop checks for the end, then read_tag's read_varint checks again.
if (p >= end) break;
p = read_tag(p, end, &tag, &err);  // read the varint, then split and check it

// After: one check for the end, inside read_tag_or_end.
TagState state;
p = read_tag_or_end(p, end, &tag, &err, &state);
if (state == TagState::End) break;
if (state == TagState::Error) return false;

// Inside read_tag_or_end, after that check, a one-byte tag:
if ((*p & 0x80) == 0) {
    field = *p >> 3;
    wire = *p & 7;
    if (field != 0 && wire != 6 && wire != 7) {
        *out = Tag{field, static_cast<WireType>(wire)};
        *state = TagState::Tag;
        return p + 1;
    }
}
```

`Dataset` gained 11% and `google_message1` 10%. That second one is a 228-byte proto2 message from
[Google's benchmark payloads](benchmarks.md#googles-benchmark-payloads).

![Chart of each benchmark's throughput relative to its reference decoder after step 4, fused tag
reads, with this step's change in color](ladder/ladder-step4.svg)

`rv-enum fx1` was 11% slower. It's the `rv fx1` pool decoded as packed enums, with one tag per
million values. It ran one more instruction per value, although its per-value code is the same in
source, and we haven't looked at the machine code for why. `rv-enum mix13`, the one- to three-byte
`rv mix13` pool as enums, was 3% slower. The streaming decoder was 5% slower on `mixed`, where an
`int32`, a `sint64`, a `double` and a string field repeat in turn, each with a handler.

## SWAR kernels for packed varints

Until this step the arena decoder read a packed varint run one byte at a time, testing each byte's
high bit to see whether the value went on. That's a branch per byte, and its direction depends on
the data. When value lengths are mixed, the branch predictor can't learn the pattern. At this point
`rv mix13`, whose varints take one to three bytes in a uniform mix, ran at 1.14 instructions per
cycle, against 4.84 on `rv fx1`.

Long runs now go to one of five new kernels, or stay with the old byte loop. The kernels are built
on SWAR, short for SIMD within a register. Eight bytes go into one 64-bit integer, and ordinary
integer instructions work on all of them at once, with no intrinsics. Two building blocks show the
technique. `bytemask8` gathers the eight high bits into one byte, a map of where values continue
and where they end. The `Struct` kernel builds that map from eight such loads, 64 bytes at a time.
Then, for each value, it loads the eight bytes where the value starts, clears those past its end
and hands them to `compact7`, which joins their seven-bit groups into the value.

```cpp
#include <cstdint>

// Bit i of the result is the high bit of byte i of w: set while a varint continues.
inline std::uint64_t bytemask8(std::uint64_t w) noexcept {
    return ((w & 0x8080808080808080ULL) * 0x0002040810204081ULL) >> 56U;
}

// Joins the low seven bits of each byte into one value, byte 0 lowest.
inline std::uint64_t compact7(std::uint64_t x) noexcept {
    x = ((x & 0x7F007F007F007F00ULL) >> 1U) | (x & 0x007F007F007F007FULL);
    x = ((x & 0x3FFF00003FFF0000ULL) >> 2U) | (x & 0x00003FFF00003FFFULL);
    x = ((x & 0x0FFFFFFF00000000ULL) >> 4U) | (x & 0x000000000FFFFFFFULL);
    return x;
}
```

![Diagram of eight input bytes, 96 01 ac 02 08 d2 85 03, read in one 64-bit load. A multiply and a
shift collect their high bits into the continuation mask 1 0 1 0 0 1 1 0. Each value's bytes then
go through three shift-and-mask rounds, giving 150, 300, 8 and 49874.](ladder/diagram-swar.svg)

For a run of 256 bytes or more, the decoder scans the first 64 bytes for where values end and picks
one of these:

- `Bulk1` when every value takes one byte. It checks 64 bytes at once for a set high bit and,
  finding none, widens each byte into one element of the array's type.
- `Bulk2` when every value takes two bytes, four to an 8-byte word.
- `Fixed` for any other single length up to 10 bytes, stepping through the run at that stride.
- `Struct` for mixed lengths up to 5 bytes, as above.
- `Swar` for mixes with a value over 5 bytes, one value per 8-byte load, with no branch for
  lengths 2 to 8.
- `ByteLoop`, the old loop, when the first value doesn't end within 64 bytes or every value there
  has one length over 10 bytes. It reports the error.

When a kernel meets a length it doesn't handle, the byte loop finishes the run, unless the kernel
had already covered a long stretch and much of the run is left, in which case the decoder picks
again. Runs under 256 bytes never reach the kernels.

`rv mix13` ran 2.1 times as fast while executing 80% more instructions per byte. Without the
per-byte branch it reached 4.3 instructions per cycle. `rv fx1` shows the reverse. Its instructions
per byte fell by two thirds and it got only 13% faster, because its high bits never vary and the
branch the kernel removed had been predicted all along. The streaming decoder didn't change.

![Chart of each benchmark's throughput relative to its reference decoder after step 5, SWAR
varint kernels, with this step's change in color](ladder/ladder-step5.svg)

The kernels make the decoder bigger. The one for long runs is compiled once per field type and
marked `RP_NOINLINE`, and flatten leaves it out of line instead of copying it into every packed
field. Picking a kernel costs the scan of the first 64 bytes, and that's why short runs skip it.

## The one-byte peek hub

After the fused tag read, every field still had its tag split into field number and wire type, then
went through a switch on the field number and a check of the wire type.

The tag of a field from 1 to 15 is a single byte that holds both. The hub, a switch at the top of
the loop, peeks at that byte before any tag is read. Each case matches one complete tag, consumes
the byte and jumps to the label where that field is decoded. Matching the case settles the field
number and the wire type together. A repeated numeric or enum field has two cases, one for a single
value and one for a packed run. Every other byte sends the field down the fused tag read's general
path, whose switch jumps into the same labels.

Here is the loop of
[`Wide`](https://github.com/VeaaC/rapidproto/blob/main/tests/bench/bench.proto), which has four
packed `int32` arrays in fields 1 to 4 and two string arrays in fields 5 and 6.
`many msgs, tiny arrays` decodes 50,000 of them, each with three values in each `int32` array and
two strings in each string field, and comes back in the next section. The diagrams after the
sketch use a smaller message whose fields 1 to 3 are `name`, `id` and `tags`.

```cpp
// Wide's decode loop with the hub, before threading, trimmed: namespace qualifiers, the field
// bodies and error checks are left out.
for (;;) {
    if (rp_c >= rp_cend) { break; }
    switch (*rp_c) {  // the next byte, not yet consumed
      case raw_tag(1, WireType::Varint): ++rp_c; goto rp_do_1;
      case raw_tag(1, WireType::Len): ++rp_c; goto rp_do_1_p;
      // ... the same two cases for fields 2, 3 and 4 ...
      case raw_tag(5, WireType::Len): ++rp_c; goto rp_do_5;
      case raw_tag(6, WireType::Len): ++rp_c; goto rp_do_6;
      default: break;
    }
    goto rp_field_general;
    rp_do_1: {  // one value of field 1, sent unpacked
        // ... append it ...
        continue;
    }
    rp_do_1_p: {  // a packed run of field 1, pre-sized and trimmed
        // ...
        continue;
    }
    // ... the labels of fields 2 to 6 ...
    rp_field_general:;
    // read_tag_or_end, then a switch on the field number: a field with a label and a
    // matching wire type jumps to it, any other field is skipped
}
```

![Diagram of the hub. The decoder peeks at one byte. 0x0a jumps to field 1 (name), 0x10 to field 2
(id) and 0x1a to field 3 (tags), while any other byte goes to the fused tag read's general
path.](ladder/diagram-hub.svg)

Compiled, the hub's switch in `Wide` is a jump table, one indirect jump per field, and so is the
switch on the field number it replaces. A message with only a few cases gets a chain of compares
instead. Where there's a table, every field still passes through one indirect jump, now with fewer
instructions around it.

`Dataset` gained 23%. `scalar records (dispatch-bound)` decodes 20,000 records of 18 scalar fields
and two sub-messages of six more, all with small values. Its instructions per byte fell 24%, and its
throughput relative to protoc rose 16%. Its bar stays grey all the same. protoc's own throughput on
this benchmark drifted by 19.8% across the builds, the most of any. A 16% change can't be told apart
from that.

![Chart of each benchmark's throughput relative to its reference decoder after step 6, the
one-byte peek hub, with this step's change in color](ladder/ladder-step6.svg)

No bar turned orange here. A field's decode is emitted once, as a label that both the hub and the
general switch jump to. The hub adds no copies of it. It does nothing for a field numbered
above 15, whose tag takes two bytes or more.

## Field-order threading

Even with the hub, every field ended by going back to it. Yet protoc writes fields in ascending
order of field number, and in its output a field is followed by another value of itself or by the
next higher-numbered field present. Protobuf doesn't promise that order, and its
[encoding guide](https://protobuf.dev/programming-guides/encoding/#order) says parsers must accept
fields in any order.

Threading ends each field's label with probes: one for each of the next two fields with labels, in
ascending order, and on a repeated field one before those for another value of its own. A probe
compares what comes next in the input with that field's tag and, on a match, jumps straight to its
label. When none matches, `continue` returns to the hub. A oneof member doesn't probe for its own
siblings, as an encoder writes only the member that's set. While the next field present is one of
those probed, the probes thread the decode from label to label without touching the hub.

A successor that the schema declares packed (proto3's default for repeated numeric and enum fields,
`[packed = true]`, or `repeated_field_encoding = PACKED` in editions) arrives as one
length-delimited run. Its probe tests that tag, and a hit enters the field's packed label,
`rp_do_<n>_p`, which fills the array and then probes for the fields after it.

```cpp
// Two of Wide's labels as shipped, trimmed: namespace qualifiers, the field bodies and error
// checks are left out.
rp_do_1: {  // one value of field 1, sent unpacked
    // ... append it ...
    // another value of field 1, or field 2 or 3 as a packed run
    if (rp_c < rp_cend && *rp_c == raw_tag(1, WireType::Varint)) { ++rp_c; goto rp_do_1; }
    if (rp_c < rp_cend && *rp_c == raw_tag(2, WireType::Len)) { ++rp_c; goto rp_do_2_p; }
    if (rp_c < rp_cend && *rp_c == raw_tag(3, WireType::Len)) { ++rp_c; goto rp_do_3_p; }
    continue;  // back to the hub
}
rp_do_1_p: {  // a packed run of field 1
    // ... fill the array, then the same probes for fields 2 and 3 ...
    continue;
}
```

![Diagram of three threaded labels. After field 1 (name) the decoder tests the next byte for 0x10
or 0x1a, after field 2 (id) for 0x1a, and after field 3 (tags) for another tags element. A miss
falls back to the hub.](ladder/diagram-threading.svg)

In machine code each probe is a compare with a constant and a direct conditional branch, two of
each for a two-byte tag. In `scalar records (dispatch-bound)` the fields always arrive in the same
order and nearly every one is present in nearly every record, so most of these branches go the same
way nearly every time. The hub's indirect jump had a different target from one field to the next.
On that benchmark threading cut instructions per byte by 26% and raised instructions per cycle from
4.1 to 5.2. It
decoded 70% faster. Relative to protoc, whose own speed on it rose 9% between the two builds, that's
56%. `google_message1` gained 23%.

![Chart of each benchmark's throughput relative to its reference decoder after step 7,
field-order threading, with this step's change in color](ladder/ladder-step7.svg)

On `many msgs, tiny arrays` the decoder ran 10% slower, and on
[`osm_blocks`](benchmarks.md#openstreetmap-data) 4% slower. `osm_blocks` holds the 246 data blocks
of an OpenStreetMap extract of Bremen, inflated before timing. In both, the fields do arrive in
ascending order, with packed arrays among them. We found the cause while writing this article. The
build we measured probed a packed successor by its element tag, the tag a single value carries when
sent unpacked. A packed run never carries that tag, so the probe always missed. In `Wide`, after
each of the first three arrays, both probes missed and the loop fell back to the hub, paying for two
compares on top of its jump. Decoding stayed correct, since the hub has a case for either form, but
instructions per byte rose 14% on `many msgs, tiny arrays` and 4% on `osm_blocks`. The fix has
since shipped and probes a packed successor by its length-delimited tag, as in the sketch above. In
a later measurement outside this series, with clang++-20, it took 13% off the arena decoder's
instructions per byte on `many msgs, tiny arrays` and 7% on `osm_blocks`. The arena decoder's own
throughput rose 16% and 6%.

## The finished decoder

At the end of the series the arena decoder still built the whole message and both decoders still
validated every field they read. The table sets the baseline build beside the last one, from before
the probe fix, for the payloads quoted above other than the `rv` pools.

| Throughput relative to the reference decoder | Reference | Baseline build | After threading |
|---|---|---|---|
| `Dataset` | protoc | 2.4 | 6.1 |
| `google_message1` | protoc | 2.2 | 3.7 |
| `scalar records (dispatch-bound)` | protoc | 0.73 | 2.1 |
| `many msgs, tiny arrays` | protoc | 1.5 | 2.5 |
| `osm_blocks` | protoc | 0.91 | 1.5 |
| `packed double(fixed)` | protoc | 0.56 | 1.1 |
| `mixed` | protozero | 1.4 | 2.3 |
| `sparse-skip` | protozero | 1.8 | 3.1 |
| `nested-msg` | protozero | 1.7 | 1.2 |

Each gain above was measured with the earlier optimizations already in place. The order shapes what
a step is worth. Two more builds from the same campaign each took one optimization back out of the
finished decoder. Forced inlining had made `Dataset` 22% faster when it came third, and in the
finished decoder it was worth 44%. On `rv fx1` it no longer mattered. The arena decoder ran that
pool at 4.8 times protoc's throughput with or without it, because the `Bulk1` kernel now does the
work that flatten did there. Borrowed strings grew the same way on `Dataset`, from 42% as the first
step to 55% at the end.

Forced inlining sped up most benchmarks. Beyond that, which optimizations matter depends on what a
schema's messages hold. Packed arrays gained most from pre-sizing and the SWAR kernels, records of
many small fields from the fused tag read, the hub and threading, and messages with many strings
from borrowing.

Every technique described here is in the C++ decoders RapidProto ships, with the probes in their
fixed form. The Rust generator uses the same techniques, with `#[inline(always)]` and
`#[inline(never)]` placed by the same flatten budget, but this series measured only the C++
decoders.
