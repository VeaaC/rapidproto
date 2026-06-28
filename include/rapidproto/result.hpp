#pragma once

// Result<T>: a success-or-error value used as the return type of every parser
// and pipeline pass. On failure it carries an Error: which source buffer, the
// byte offset into that source, and a human-readable message. (The byte offset
// could be turned into line:col from the source text, though no renderer currently
// does so — the CLI prints the message as-is.)
//
// RP_TRY(target, expr) unwraps a Result: on error it returns the error from the
// enclosing function (which must itself return some Result<...>); on success it
// binds `target` to a reference to the contained value.

#include <cstddef>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

#include "rapidproto/range.hpp"
#include "rapidproto/source_id.hpp"

namespace rapidproto {

struct Error {
    SourceId source;              // which source buffer; invalid() if not tied to one
    std::size_t byte_offset = 0;  // offset into that source's text
    std::string message;
    // A "committed" error: combinators that normally backtrack (alt/many/opt/...)
    // propagate it instead of swallowing it. Set by the cut() combinator.
    bool fatal = false;

    Error() = default;

    // Single-source convenience: the lexer/parser work on one buffer, so an offset
    // and message suffice (source stays invalid; a caller that knows the buffer can
    // stamp it later).
    Error(std::size_t offset, std::string msg) : byte_offset(offset), message(std::move(msg)) {}

    // Cross-file: explicitly identify the source the offset indexes into.
    Error(SourceId src, std::size_t offset, std::string msg)
        : source(src), byte_offset(offset), message(std::move(msg)) {}
};

template <typename T>
class [[nodiscard]] Result {
    // Result<Error> would make the two constructors below ambiguous (and form a
    // variant<Error, Error>); forbid it with a clear diagnostic. Also note: T must
    // not be implicitly constructible from Error, or `return error;` could quietly
    // build a success value instead.
    static_assert(!std::is_same_v<T, Error>,
                  "Result<Error> is ill-formed; wrap the error in a distinct success type.");

public:
    // Implicit on purpose: lets parsers `return value;` / `return error;`.
    Result(T value) : m_data(std::move(value)) {}
    Result(Error error) : m_data(std::move(error)) {}

    bool is_ok() const noexcept { return std::holds_alternative<T>(m_data); }
    bool is_err() const noexcept { return std::holds_alternative<Error>(m_data); }
    explicit operator bool() const noexcept { return is_ok(); }

    // Precondition: is_ok(). Accessing value() on an error throws std::bad_variant_access.
    T& value() & { return std::get<T>(m_data); }
    const T& value() const& { return std::get<T>(m_data); }
    T&& value() && { return std::get<T>(std::move(m_data)); }

    // Precondition: is_err().
    Error& error() & { return std::get<Error>(m_data); }
    const Error& error() const& { return std::get<Error>(m_data); }
    Error&& error() && { return std::get<Error>(std::move(m_data)); }

private:
    std::variant<T, Error> m_data;
};

// A parser's success payload: the value it produced plus the unconsumed input.
template <typename Output, typename Input>
struct Parsed {
    Range<Input> remaining;
    Output value;
};

}  // namespace rapidproto

// RP_TRY declares variables in the CALLER's scope, so it must appear as a
// standalone statement inside a braced block — never as the unbraced body of an
// if/for/while. The enclosing function must return some Result<...> (the error
// is forwarded by implicit Result(Error) conversion).
//
// Two-level concatenation so the unique-id token expands before pasting; the id
// is __COUNTER__ (not __LINE__) so two RP_TRYs on the same physical line don't
// collide. __COUNTER__ is captured once via the IMPL indirection so all three
// pastes share the same value.
#define RP_CONCAT_(a, b) a##b
#define RP_CONCAT(a, b) RP_CONCAT_(a, b)

// clang-format off
#define RP_TRY(TARGET, EXPR) RP_TRY_IMPL(TARGET, EXPR, __COUNTER__)
#define RP_TRY_IMPL(TARGET, EXPR, ID)                                              \
    auto RP_CONCAT(rp_try_tmp_, ID) = (EXPR);                                      \
    if (!RP_CONCAT(rp_try_tmp_, ID)) {                                             \
        return std::move(RP_CONCAT(rp_try_tmp_, ID).error());                      \
    }                                                                              \
    auto& TARGET = RP_CONCAT(rp_try_tmp_, ID).value()
// clang-format on
