// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Christian Vetter
#pragma once

// A protoc message created IN a google::protobuf::Arena, on every protobuf release the benchmarks
// build against. Before v26, Arena::Create default-constructs a message without the arena: only the
// object lands in it, its strings and sub-messages go to the heap, and CreateMessage is the
// arena-aware call. From v26 Create is arena-aware too; v27 to v29 mark CreateMessage deprecated,
// which the pragmas below keep out of the build output, and v30 removed it. Wherever CreateMessage
// exists it is arena-aware, so this prefers it and falls back on Create.

#include <type_traits>

#include <google/protobuf/arena.h>

namespace rpbaseline {

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

template <class Message, class A, class = void>
struct HasCreateMessage : std::false_type {};
template <class Message, class A>
struct HasCreateMessage<Message, A,
                        std::void_t<decltype(A::template CreateMessage<Message>(nullptr))>>
    : std::true_type {};

template <class Message, class A = google::protobuf::Arena>
Message* new_in_arena(A* arena) {
    if constexpr (HasCreateMessage<Message, A>::value) {
        return A::template CreateMessage<Message>(arena);
    } else {
        return A::template Create<Message>(arena);
    }
}

#pragma GCC diagnostic pop

}  // namespace rpbaseline
