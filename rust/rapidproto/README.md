# rapidproto

`rapidproto` is the runtime for RapidProto's Rust decoders. The code that
[`rapidproto-build`][build] generates from your Protobuf schemas calls into it, so you add this
crate next to `rapidproto-build`. You can read a decoded message but not change or encode it, and
there's no reflection. The [benchmark page][bench] compares RapidProto's speed with prost,
rust-protobuf, quick-protobuf, femtopb and google-protobuf.

`rapidproto-build` runs in your build script without protoc. Its page has the setup, with the `=`
pins that keep the two crates on one release. These are test releases, and the API may change from
one version to the next.

## Reading a message

The example on `rapidproto-build`'s page puts the generated code in a module named `schema`, with a
decoder for a `shop.Order` message. Reading an order with it looks like this:

```rust
use schema::arena::shop::Order;

fn describe(input: &[u8]) -> Result<String, Box<dyn std::error::Error>> {
    let arena = rapidproto::Arena::new();
    arena.set_capacity_limit(1 << 20); // 1 MiB, for untrusted input
    let order = Order::decode(input, &arena)?; // borrows input and arena

    let id: &str = order.id().to_str()?; // checks UTF-8
    let quantities: &[i64] = order.quantities();
    Ok(format!("order {id}: {} items", quantities.iter().sum::<i64>()))
}
```

The example uses the [arena decoder][root]. `decode` stores the message in the arena, and strings
and bytes borrow from the input. A crafted input can make the arena many times larger than itself.
The capacity limit is a fixed ceiling, not a multiple of the input, so leave room for the largest
message you mean to accept ([untrusted input][untrusted]).

The generator writes the [streaming decoder][stream] too if you ask for it, with `.stream(true)` in
the build script. Instead of building a message, it hands each field you selected to your handlers
and skips over the others.

## `no_std` and features

The crate is `no_std`. Its default `alloc` feature adds `Arena::new`, whose arena takes memory from
the heap as it fills. It also adds `Order::decode_owned`. The `Owned` it returns holds the message
with its own input and arena, so you can keep it as long as you like. That arena has no capacity
limit, so decode untrusted input into an arena of your own, as in the example.

Turn `alloc` off and you make an arena with [`Arena::with_buffer`][with-buffer] over memory you
provide. A decode fails once that memory runs out.

The [`serde`][serde] feature, together with `.serde(true)` in the build script, gives the generated
messages and enums `serde::Serialize` impls, but no `Deserialize`. Unless you turn `serde` on,
`rapidproto` doesn't depend on any other crate.

`rapidproto` is under the Apache-2.0 license ([LICENSE][license], [NOTICE][notice]).

[build]: https://crates.io/crates/rapidproto-build
[root]: https://docs.rs/rapidproto
[untrusted]: https://docs.rs/rapidproto/latest/rapidproto/#untrusted-input
[bench]: https://github.com/VeaaC/rapidproto/blob/main/docs/benchmarks.md#rust
[stream]: https://docs.rs/rapidproto/latest/rapidproto/stream/index.html
[with-buffer]: https://docs.rs/rapidproto/latest/rapidproto/struct.Arena.html#method.with_buffer
[serde]: https://docs.rs/rapidproto/latest/rapidproto/#serde
[license]: https://github.com/VeaaC/rapidproto/blob/main/rust/rapidproto/LICENSE
[notice]: https://github.com/VeaaC/rapidproto/blob/main/rust/rapidproto/NOTICE
