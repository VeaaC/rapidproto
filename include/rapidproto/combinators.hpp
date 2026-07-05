#pragma once

// Parser combinators (header-only).
//
// A *parser* is any callable  Range<I> -> Result<Parsed<O, I>>.
//   - On success it returns the produced value O plus the unconsumed remainder
//     (a subspan of the input).
//   - On failure it returns an Error whose byte_offset is the failure position
//     measured RELATIVE TO THE INPUT RANGE the parser was given. For Range<char>
//     that is the byte offset; sequential combinators (seq, preceded, delimited,
//     separated_list) lift a child's relative offset by the amount already
//     consumed, so the offset reported by the outermost parser is relative to the
//     buffer it was handed — i.e. absolute when invoked on the whole source.
//
// (For the token parser the element offset is a token index, not a byte offset;
// mapping it back to a source byte offset is handled by the import resolver.)
//
// Most combinators return a generic lambda templated on the input range, so they
// work for both Range<char> (lexer) and Range<Token> (parser). `tag` and
// `take_until` are char-specific (they match character subsequences).

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "rapidproto/range.hpp"
#include "rapidproto/result.hpp"

namespace rapidproto {
namespace detail {

// Elements consumed when `after` is a tail subspan of `before` (same buffer).
template <typename I>
constexpr std::size_t consumed(Range<I> before, Range<I> after) {
    return static_cast<std::size_t>(after.data() - before.data());
}

// Result<Parsed<O, I>> -> O / I.
template <typename R>
struct parse_traits;
template <typename O, typename I>
struct parse_traits<Result<Parsed<O, I>>> {
    using output = O;
    using input = I;
};

// The output type O of parser P when invoked on input range In (= Range<I>).
template <typename P, typename In>
using output_t = typename parse_traits<std::invoke_result_t<P, In>>::output;

// seq: run parsers left-to-right, accumulating a tuple of their outputs, and
// translate any child error offset to be relative to the original input.
// R is the (fixed) final result type Result<Parsed<tuple<...>, I>>.
template <typename R, typename I, typename Acc, typename P0, typename... Ps>
R seq_run(Range<I> rest, std::size_t base, Acc acc, const P0& p0, const Ps&... ps) {
    auto r = p0(rest);
    if (!r) {
        Error e = std::move(r.error());
        e.byte_offset += base;
        return R(std::move(e));
    }
    auto& ok = r.value();
    const std::size_t adv = consumed(rest, ok.remaining);
    auto next = std::tuple_cat(std::move(acc), std::tuple<decltype(ok.value)>(std::move(ok.value)));
    if constexpr (sizeof...(Ps) == 0) {
        return R(Parsed<decltype(next), I>{ok.remaining, std::move(next)});
    } else {
        return seq_run<R>(ok.remaining, base + adv, std::move(next), ps...);
    }
}

// alt: return the first success, else the error from the branch that got farthest
// (largest byte_offset). All branches see the same input, so offsets compare
// directly; ties favor the earlier branch (its message is usually the "primary").
template <typename R, typename I, typename P0, typename... Ps>
R alt_run(Range<I> in, const P0& p0, const Ps&... ps) {
    R r = p0(in);
    if (r) {
        return r;
    }
    if (r.error().fatal) {
        return r;  // committed (cut) failure: do not try the other branches
    }
    if constexpr (sizeof...(Ps) == 0) {
        return r;  // sole remaining branch failed; its error is the farthest so far
    } else {
        Error first_err = std::move(r.error());
        R rest = alt_run<R>(in, ps...);
        if (rest) {
            return rest;  // a later branch succeeded
        }
        if (rest.error().fatal) {
            return rest;  // a later branch committed-failed
        }
        if (first_err.byte_offset >= rest.error().byte_offset) {
            return R(std::move(first_err));
        }
        return rest;
    }
}

}  // namespace detail

// --- Primitives ------------------------------------------------------------

// Match a single element satisfying `pred`; produce that element.
template <typename Pred>
auto one(Pred pred) {
    return [pred](auto in) {
        using I = typename decltype(in)::value_type;
        using Ok = Parsed<I, I>;
        if (!in.empty() && pred(in.front())) {
            return Result<Ok>(Ok{in.subspan(1), in.front()});
        }
        return Result<Ok>(Error{0, "unexpected input"});
    };
}

// Match an exact character subsequence; produce the matched span. (char-only)
inline auto tag(std::string_view expected) {
    return [expected](Range<char> in) -> Result<Parsed<Range<char>, char>> {
        using Ok = Parsed<Range<char>, char>;
        if (in.size() >= expected.size() &&
            in.subspan(0, expected.size()).to_string_view() == expected) {
            return Ok{in.subspan(expected.size()), in.subspan(0, expected.size())};
        }
        return Error{0, "expected \"" + std::string(expected) + "\""};
    };
}

// Greedily consume 0+ elements satisfying `pred`; produce the consumed span.
template <typename Pred>
auto take_while(Pred pred) {
    return [pred](auto in) {
        using I = typename decltype(in)::value_type;
        using Ok = Parsed<Range<I>, I>;
        std::size_t n = 0;
        while (n < in.size() && pred(in[n])) {
            ++n;
        }
        return Result<Ok>(Ok{in.subspan(n), in.subspan(0, n)});
    };
}

// Greedily consume 1+ elements satisfying `pred`; fail if none match.
template <typename Pred>
auto take_while1(Pred pred) {
    return [pred](auto in) {
        using I = typename decltype(in)::value_type;
        using Ok = Parsed<Range<I>, I>;
        std::size_t n = 0;
        while (n < in.size() && pred(in[n])) {
            ++n;
        }
        if (n == 0) {
            return Result<Ok>(Error{0, "expected at least one matching element"});
        }
        return Result<Ok>(Ok{in.subspan(n), in.subspan(0, n)});
    };
}

// Consume 0+ elements until `pred` is true (exclusive); produce the consumed span.
template <typename Pred>
auto take_till(Pred pred) {
    return [pred](auto in) {
        using I = typename decltype(in)::value_type;
        using Ok = Parsed<Range<I>, I>;
        std::size_t n = 0;
        while (n < in.size() && !pred(in[n])) {
            ++n;
        }
        return Result<Ok>(Ok{in.subspan(n), in.subspan(0, n)});
    };
}

// Consume up to (not including) the first occurrence of `needle`; produce the
// span before it. Fails if `needle` is absent — and (unlike the other primitives,
// which report offset 0 on failure) reports the failure at in.size(), since the
// needle is "missing at the end of the input". (char-only)
inline auto take_until(std::string_view needle) {
    return [needle](Range<char> in) -> Result<Parsed<Range<char>, char>> {
        using Ok = Parsed<Range<char>, char>;
        const std::string_view hay = in.to_string_view();
        const std::size_t pos = hay.find(needle);
        if (pos == std::string_view::npos) {
            return Error{in.size(), "expected \"" + std::string(needle) + "\""};
        }
        return Ok{in.subspan(pos), in.subspan(0, pos)};
    };
}

// --- Combinators -----------------------------------------------------------

// Try parsers in order; first success wins. On all-fail, return the farthest error.
template <typename... Ps>
auto alt(Ps... ps) {
    static_assert(sizeof...(Ps) >= 1, "alt needs at least one parser");
    return [ps...](auto in) {
        using I = typename decltype(in)::value_type;
        using First = std::tuple_element_t<0, std::tuple<Ps...>>;
        using R = std::invoke_result_t<First, Range<I>>;
        static_assert((std::is_same_v<R, std::invoke_result_t<Ps, Range<I>>> && ...),
                      "all alt branches must have the same output and input types");
        return detail::alt_run<R>(in, ps...);
    };
}

// Run all parsers in sequence; produce a tuple of their outputs.
template <typename... Ps>
auto seq(Ps... ps) {
    static_assert(sizeof...(Ps) >= 1, "seq needs at least one parser");
    return [ps...](auto in) {
        using I = typename decltype(in)::value_type;
        using Tup = std::tuple<detail::output_t<Ps, Range<I>>...>;
        using R = Result<Parsed<Tup, I>>;
        return detail::seq_run<R>(in, std::size_t{0}, std::tuple<>{}, ps...);
    };
}

// Commit: mark a parser's failure as fatal so the backtracking combinators
// (alt/many/many1/opt/separated_list) propagate it instead of swallowing it. Use
// it once a construct is unambiguously entered (e.g. after an opening quote) so a
// later failure becomes a precise, propagating error rather than a silent backtrack.
template <typename P>
auto cut(P p) {
    return [p](auto in) {
        auto r = p(in);
        if (!r) {
            r.error().fatal = true;
        }
        return r;
    };
}

// Make a parser optional; never fails (unless the inner parser fails fatally, via
// cut). Produces std::optional<O>.
template <typename P>
auto opt(P p) {
    return [p](auto in) {
        using I = typename decltype(in)::value_type;
        using O = detail::output_t<P, Range<I>>;
        using Ok = Parsed<std::optional<O>, I>;
        auto r = p(in);
        if (r) {
            return Result<Ok>(
                Ok{r.value().remaining, std::optional<O>(std::move(r.value().value))});
        }
        if (r.error().fatal) {
            return Result<Ok>(std::move(r.error()));
        }
        return Result<Ok>(Ok{in, std::optional<O>(std::nullopt)});
    };
}

// 0+ repetitions; produces a vector<O>. Guards against a sub-parser that succeeds
// without consuming input (which would loop forever).
template <typename P>
auto many(const P& p) {
    return [p](auto in) {
        using I = typename decltype(in)::value_type;
        using O = detail::output_t<P, Range<I>>;
        using Ok = Parsed<std::vector<O>, I>;
        std::vector<O> out;
        Range<I> rest = in;
        std::size_t base = 0;
        for (;;) {
            auto r = p(rest);
            if (!r) {
                if (r.error().fatal) {
                    Error e = std::move(r.error());
                    e.byte_offset += base;  // lift the committed error to this parser's input
                    return Result<Ok>(std::move(e));
                }
                break;
            }
            const std::size_t adv = detail::consumed(rest, r.value().remaining);
            if (adv == 0) {
                return Result<Ok>(Error{base, "many: sub-parser consumed no input"});
            }
            out.push_back(std::move(r.value().value));
            rest = r.value().remaining;
            base += adv;
        }
        return Result<Ok>(Ok{rest, std::move(out)});
    };
}

// 1+ repetitions; fails (forwarding the sub-parser's error) if none match.
template <typename P>
auto many1(P p) {
    return [p](auto in) {
        using I = typename decltype(in)::value_type;
        using O = detail::output_t<P, Range<I>>;
        using Ok = Parsed<std::vector<O>, I>;
        std::vector<O> out;
        Range<I> rest = in;
        std::size_t base = 0;
        auto first = p(rest);
        if (!first) {
            return Result<Ok>(std::move(first.error()));
        }
        {
            // Seed with the first element (parsed exactly once).
            const std::size_t adv = detail::consumed(rest, first.value().remaining);
            if (adv == 0) {
                return Result<Ok>(Error{base, "many1: sub-parser consumed no input"});
            }
            out.push_back(std::move(first.value().value));
            rest = first.value().remaining;
            base += adv;
        }
        for (;;) {
            auto r = p(rest);
            if (!r) {
                if (r.error().fatal) {
                    Error e = std::move(r.error());
                    e.byte_offset += base;  // lift the committed error to this parser's input
                    return Result<Ok>(std::move(e));
                }
                break;
            }
            const std::size_t adv = detail::consumed(rest, r.value().remaining);
            if (adv == 0) {
                return Result<Ok>(Error{base, "many1: sub-parser consumed no input"});
            }
            out.push_back(std::move(r.value().value));
            rest = r.value().remaining;
            base += adv;
        }
        return Result<Ok>(Ok{rest, std::move(out)});
    };
}

// Transform a parser's output with `f`.
template <typename P, typename F>
auto map(const P& p, F f) {
    return [p, f](auto in) {
        using I = typename decltype(in)::value_type;
        using O = detail::output_t<P, Range<I>>;
        using O2 = std::decay_t<std::invoke_result_t<F, O>>;
        using Ok = Parsed<O2, I>;
        auto r = p(in);
        if (!r) {
            return Result<Ok>(std::move(r.error()));
        }
        return Result<Ok>(Ok{r.value().remaining, f(std::move(r.value().value))});
    };
}

// Discard a parser's value and produce the span of input it consumed instead.
template <typename P>
auto recognize(P p) {
    return [p](auto in) {
        using I = typename decltype(in)::value_type;
        using Ok = Parsed<Range<I>, I>;
        auto r = p(in);
        if (!r) {
            return Result<Ok>(std::move(r.error()));
        }
        const std::size_t adv = detail::consumed(in, r.value().remaining);
        return Result<Ok>(Ok{r.value().remaining, in.subspan(0, adv)});
    };
}

// Succeed only if the parser consumes the entire input.
template <typename P>
auto all_consuming(P p) {
    return [p](auto in) {
        auto r = p(in);
        if (!r) {
            return r;
        }
        if (!r.value().remaining.empty()) {
            const std::size_t off = detail::consumed(in, r.value().remaining);
            return decltype(r)(Error{off, "unexpected trailing input"});
        }
        return r;
    };
}

// Run pre, mid, suf in sequence; produce mid's value.
template <typename Pre, typename Mid, typename Suf>
auto delimited(Pre pre, const Mid& mid, Suf suf) {
    return [pre, mid, suf](auto in) {
        using I = typename decltype(in)::value_type;
        using O = detail::output_t<Mid, Range<I>>;
        using Ok = Parsed<O, I>;
        auto r1 = pre(in);
        if (!r1) {
            return Result<Ok>(std::move(r1.error()));
        }
        const std::size_t b1 = detail::consumed(in, r1.value().remaining);
        auto r2 = mid(r1.value().remaining);
        if (!r2) {
            Error e = std::move(r2.error());
            e.byte_offset += b1;
            return Result<Ok>(std::move(e));
        }
        const std::size_t b2 = b1 + detail::consumed(r1.value().remaining, r2.value().remaining);
        auto r3 = suf(r2.value().remaining);
        if (!r3) {
            Error e = std::move(r3.error());
            e.byte_offset += b2;
            return Result<Ok>(std::move(e));
        }
        return Result<Ok>(Ok{r3.value().remaining, std::move(r2.value().value)});
    };
}

// Run pre then body; produce body's value (discarding pre's).
template <typename Pre, typename Body>
auto preceded(Pre pre, Body body) {
    return [pre, body](auto in) {
        using I = typename decltype(in)::value_type;
        using O = detail::output_t<Body, Range<I>>;
        using Ok = Parsed<O, I>;
        auto r1 = pre(in);
        if (!r1) {
            return Result<Ok>(std::move(r1.error()));
        }
        const std::size_t b1 = detail::consumed(in, r1.value().remaining);
        auto r2 = body(r1.value().remaining);
        if (!r2) {
            Error e = std::move(r2.error());
            e.byte_offset += b1;
            return Result<Ok>(std::move(e));
        }
        return Result<Ok>(Ok{r2.value().remaining, std::move(r2.value().value)});
    };
}

// 0+ items separated by `sep`: item (sep item)*. Produces a vector<O>. A trailing
// separator (sep with no following item) is an error.
template <typename Item, typename Sep>
auto separated_list(Item item, Sep sep) {
    return [item, sep](auto in) {
        using I = typename decltype(in)::value_type;
        using O = detail::output_t<Item, Range<I>>;
        using Ok = Parsed<std::vector<O>, I>;
        std::vector<O> out;
        Range<I> rest = in;
        std::size_t base = 0;
        auto first = item(rest);
        if (!first) {
            if (first.error().fatal) {
                return Result<Ok>(std::move(first.error()));
            }
            return Result<Ok>(Ok{in, std::move(out)});  // empty list
        }
        base += detail::consumed(rest, first.value().remaining);
        out.push_back(std::move(first.value().value));
        rest = first.value().remaining;
        for (;;) {
            auto s = sep(rest);
            if (!s) {
                if (s.error().fatal) {
                    Error e = std::move(s.error());
                    e.byte_offset += base;
                    return Result<Ok>(std::move(e));
                }
                break;
            }
            const std::size_t sadv = detail::consumed(rest, s.value().remaining);
            auto it = item(s.value().remaining);
            if (!it) {
                if (!it.error().fatal && sadv == 0) {
                    break;  // zero-width separator: nothing truly separated us — stop here
                }
                // committed failure, or a real separator with no item following it
                Error e = std::move(it.error());
                e.byte_offset += base + sadv;
                return Result<Ok>(std::move(e));
            }
            const std::size_t iadv = detail::consumed(s.value().remaining, it.value().remaining);
            if (sadv + iadv == 0) {
                break;  // progress guard
            }
            out.push_back(std::move(it.value().value));
            rest = it.value().remaining;
            base += sadv + iadv;
        }
        return Result<Ok>(Ok{rest, std::move(out)});
    };
}

}  // namespace rapidproto
