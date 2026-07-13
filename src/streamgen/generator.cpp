#include "rapidproto/streamgen/generator.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "rapidproto/ast.hpp"
#include "rapidproto/codegen/emit.hpp"
#include "rapidproto/codegen/naming.hpp"
#include "rapidproto/codegen/printer.hpp"
#include "rapidproto/codegen/wire.hpp"
#include "rapidproto/version.hpp"

namespace rapidproto::streamgen {

// The C++-naming + printer layer lives in the neutral rapidproto::codegen module (shared with
// arenagen); pull the pieces this emitter uses into scope.
using codegen::build_cpp_names;
using codegen::cpp_type_name;
using codegen::CppNameTable;
using codegen::namespace_of;
using codegen::Printer;

namespace {

// How one field maps to generated code: the C++ Value delivered to the callback, the wire type its
// arm matches, and the expression turning the raw value-threaded read (a fresh local) into the Value
// (decode_pre + read-local + decode_post).
struct FieldGen {
    std::string value_type;
    std::string_view wire_type;  // WireType enumerator
    std::string decode_pre;      // wraps the raw read value -> Value
    std::string decode_post;
    // For a Varint-wire field: the NAMED rapidproto::wire:: conversion functor the packed kernel is
    // templated on (so the whole packed span is decoded by the shared decode_packed_varints; see
    // runtime.hpp). Empty for non-Varint fields (their packed path stays per-element).
    std::string packed_conv;
};

std::optional<FieldGen> scalar_gen(std::string_view type) {
    // Wire type / read call / value conversion are the shared codegen facts (codegen/wire.hpp). The
    // callback Value type is streamgen's own: numeric/bool scalars use the shared C++-type mapping;
    // string/bytes deliver a std::string_view view into the wire buffer. nullopt = not a scalar.
    const codegen::ScalarWire* w = codegen::find_scalar_wire(type);
    if (w == nullptr) {
        return std::nullopt;
    }
    const bool is_str = type == "string" || type == "bytes";
    std::string value_type =
        is_str ? "std::string_view" : std::string(codegen::cpp_numeric_type(type));
    return FieldGen{std::move(value_type), w->wire, std::string(w->pre), std::string(w->post),
                    std::string(w->packed_conv)};
}

// How to decode one scalar / enum / message / group field (the Value the callback receives).
FieldGen field_gen(const CppNameTable& symbols, const FieldNode& field) {
    if (std::optional<FieldGen> scalar = scalar_gen(field.type_name)) {
        return *scalar;
    }
    const std::string cpp = cpp_type_name(symbols, field.resolved_type_fqn);
    if (field.is_enum_type) {
        return {cpp, "Varint", "static_cast<" + cpp + ">(::rapidproto::varint_to_int32(", "))",
                "::rapidproto::wire::conv_enum<" + cpp + ">"};
    }
    if (field.message_encoding == MessageEncoding::Delimited) {
        // Delimited wire format (a proto2 `group` or an editions DELIMITED message field): the
        // Value is the sub-decoder over the body delimited by SGROUP/EGROUP. (`is_group` only marks
        // the synthesized-nested-message structure; the wire form is decided by
        // `message_encoding`.)
        return {cpp, "SGroup", cpp + "{", "}", ""};
    }
    // message-typed: the Value is the sub-decoder, constructed over the LEN payload.
    return {cpp, "Len", cpp + "{", "}", ""};
}

// How to decode a map field's value (scalar / enum / message). The key is a scalar (analyze checks).
FieldGen map_value_gen(const CppNameTable& symbols, const MapFieldNode& map) {
    if (std::optional<FieldGen> scalar = scalar_gen(map.value_type)) {
        return *scalar;
    }
    const std::string cpp = cpp_type_name(symbols, map.resolved_value_type_fqn);
    if (map.value_is_enum) {
        return {cpp, "Varint", "static_cast<" + cpp + ">(::rapidproto::varint_to_int32(", "))", ""};
    }
    return {cpp, "Len", cpp + "{", "}", ""};  // message value
}

// A scalar / enum / message / group field (singular or repeated). Maps live in map_fields and are
// handled separately. [[maybe_unused]] because its only caller is an assert(), which compiles out
// under NDEBUG (where it would otherwise be an unused-function error).
[[maybe_unused]] bool is_generatable(const FieldNode& field) {
    return scalar_gen(field.type_name).has_value() || field.is_enum_type || field.is_message_type;
}

// The generatable fields of a message, in wire-dispatch order: declared fields then oneof members
// (oneof members are plain fields). Map fields are handled separately.
std::vector<std::pair<const FieldNode*, FieldGen>> collect_fields(const CppNameTable& symbols,
                                                                  const MessageNode& message) {
    std::vector<std::pair<const FieldNode*, FieldGen>> fields;
    const auto collect = [&](const FieldNode& field) {
        // A resolved field is always a scalar, enum, or message (the resolver guarantees this). A
        // field that is none would silently vanish from the generated struct -- assert so a future
        // AST construct (a new field kind) can't be lost without notice.
        assert(is_generatable(field) && "streamgen: a resolved field must be scalar/enum/message");
        fields.emplace_back(&field, field_gen(symbols, field));
    };
    for (const auto& field : message.fields) {
        collect(field);
    }
    for (const auto& oneof : message.oneofs) {
        for (const auto& field : oneof.fields) {
            collect(field);
        }
    }
    return fields;
}

// Value-threaded read emitters (defined below, with the general field arms). The map arm here uses
// them, so forward-declare. Each threads the wire cursor by value through the rapidproto::wire:: free functions
// (runtime.hpp) instead of a WireReader member, keeping it in registers across the decode loop.
std::string emit_vt_read(Printer& printer, const FieldGen& gen, const std::string& cur,
                         const std::string& end, const std::string& beg);
void emit_vt_read_into(Printer& printer, const FieldGen& gen, const std::string& target,
                       const std::string& cur, const std::string& end, const std::string& beg);
void emit_vt_len_read(Printer& printer, const std::string& view);

void emit_map_tag(Printer& printer, const CppNameTable& symbols, const MapFieldNode& map) {
    const std::optional<FieldGen> key_gen = scalar_gen(map.key_type);
    if (!key_gen) {
        return;  // unreachable: analyze() rejects a non-scalar map key before codegen. Guard rather
                 // than deref a disengaged optional, so a logic error degrades to a no-op, not UB.
    }
    const FieldGen& key = *key_gen;
    const FieldGen value = map_value_gen(symbols, map);
    printer.print(
        "struct $f$ { using Key = $kt$; using Value = $vt$; static constexpr std::uint32_t"
        " kNumber = $n$; static constexpr std::string_view kName = \"$real$\"; };\n",
        {{"f", symbols.local.at(&map)},
         {"kt", key.value_type},
         {"vt", value.value_type},
         {"n", std::to_string(map.number)},
         {"real", map.name}});
}

// A map field decodes as repeated `{ key=1, value=2 }` LEN entries; the callback fires once per
// entry as (Tag, Key, Value). Absent key/value default to their zero value.
void emit_map_arm(Printer& printer, const CppNameTable& symbols, const MapFieldNode& map) {
    const std::string fname = symbols.local.at(&map);
    const std::optional<FieldGen> key_gen = scalar_gen(map.key_type);
    if (!key_gen) {
        return;  // unreachable: analyze() rejects a non-scalar map key before codegen. Guard rather
                 // than deref a disengaged optional, so a logic error degrades to a no-op, not UB.
    }
    const FieldGen& key = *key_gen;
    const FieldGen value = map_value_gen(symbols, map);
    // A message-typed map value is a sub-decoder constructed over a ByteView; default it to an
    // empty ByteView so an entry with no value field still yields a valid (empty) sub-message.
    const std::string value_default = map.value_is_message ? "::rapidproto::ByteView{}" : "";

    printer.print("case $f$::kNumber:\n", {{"f", fname}});
    printer.indent();
    codegen::emit_dispatch_guards(
        printer, "Callbacks", fname + ", " + fname + "::Key, " + fname + "::Value",
        "map field '" + fname + "'", fname + "::Key, " + fname + "::Value");
    printer.print(
        "if constexpr ((false || ... ||"
        " ::rapidproto::handles_one<Callbacks, $f$, $f$::Key, $f$::Value>)) {\n",
        {{"f", fname}});
    printer.indent();
    printer.print("if (rp_tag.wire_type == ::rapidproto::WireType::Len) {\n");
    printer.indent();
    emit_vt_len_read(printer, "rp_entry");  // the entry payload, read from the main cursor rp_c
    printer.print("$f$::Key rp_key{};\n", {{"f", fname}});
    printer.print("$f$::Value rp_value{$d$};\n", {{"f", fname}, {"d", value_default}});
    // Value-threaded entry loop: thread a byte cursor over the entry payload (stays in registers).
    // Offsets are entry-payload-relative; rp_we is the decode()'s shared wire-error slot. The offset
    // base equals byte_ptr(rp_entry) (rp_ec's initial value); recompute it on the cold fail paths
    // rather than hold it live across the hot loop (a free reinterpret_cast off rp_entry).
    printer.print("const std::uint8_t* rp_ec = ::rapidproto::wire::byte_ptr(rp_entry);\n");
    printer.print("const std::uint8_t* const rp_ee = rp_ec + rp_entry.size();\n");
    const std::string ebeg = "::rapidproto::wire::byte_ptr(rp_entry)";
    printer.print("::rapidproto::Tag rp_et{};\n");
    printer.print("for (;;) {\n");
    printer.indent();
    printer.print("::rapidproto::wire::TagState rp_es = ::rapidproto::wire::TagState::End;\n");
    printer.print(
        "const std::uint8_t* const rp_etp ="  // entry-loop tag ptr (distinct from the outer rp_tp)
        " ::rapidproto::wire::read_tag_or_end(rp_ec, rp_ee, &rp_et, &rp_we, &rp_es);\n");
    printer.print("if (rp_es == ::rapidproto::wire::TagState::End) { break; }\n");
    printer.print(
        "if (rp_es == ::rapidproto::wire::TagState::Error) { return "
        "::rapidproto::DecodeStatus{rp_we,"
        " false, static_cast<std::size_t>(rp_ec - " +
        ebeg + ")}; }\n");
    printer.print("rp_ec = rp_etp;\n");
    printer.print(
        "if (rp_et.field_number == 1 && rp_et.wire_type == ::rapidproto::WireType::$kw$) {\n",
        {{"kw", key.wire_type}});
    printer.indent();
    emit_vt_read_into(printer, key, "rp_key", "rp_ec", "rp_ee", ebeg);
    printer.outdent();
    printer.print(
        "} else if (rp_et.field_number == 2 &&"
        " rp_et.wire_type == ::rapidproto::WireType::$vw$) {\n",
        {{"vw", value.wire_type}});
    printer.indent();
    emit_vt_read_into(printer, value, "rp_value", "rp_ec", "rp_ee", ebeg);
    printer.outdent();
    printer.print("} else {\n");
    printer.indent();
    printer.print("std::size_t rp_efo = 0;\n");
    printer.print(
        "const std::uint8_t* const rp_esp ="
        " ::rapidproto::wire::skip_value(rp_ec, rp_ee, " +
        ebeg + ", rp_et, 0, &rp_we, &rp_efo);\n");
    printer.print(
        "if (rp_esp == nullptr) { return ::rapidproto::DecodeStatus{rp_we, false, rp_efo}; }\n");
    printer.print("rp_ec = rp_esp;\n");
    printer.outdent();
    printer.print("}\n");
    printer.outdent();
    printer.print("}\n");  // for (;;) map entry
    printer.print(
        "if (const auto rp_status = ::rapidproto::invoke_field(rp_dispatch, $f${}, rp_key, "
        "rp_value);"
        " !rp_status.ok()) {\n",
        {{"f", fname}});
    printer.indent();
    printer.print("return rp_status;\n");
    printer.outdent();
    printer.print("}\n");
    printer.print("continue;\n");  // entry consumed -> continue the field loop
    printer.outdent();
    printer.print("}\n");  // if Len
    printer.outdent();
    printer.print("}\n");       // handles
    printer.print("break;\n");  // not consumed -> shared skip after the switch
    printer.outdent();
}

// Every message or enum `message`'s subtree references, by FQN. Each appears in a field tag's
// `using Value = <type>`. Naming a type NESTED inside a sibling needs that sibling complete (a nested
// type has no out-of-class forward declaration), so the sibling must be emitted first; a direct-sibling
// reference needs only the forward declaration, so depends_on only orders on a strictly-nested target.
void collect_referenced_types(const MessageNode& message, std::unordered_set<std::string>& out) {
    for (const auto& field : message.fields) {
        if (field.is_message_type || field.is_enum_type) {
            out.insert(field.resolved_type_fqn);
        }
    }
    for (const auto& oneof : message.oneofs) {
        for (const auto& field : oneof.fields) {
            if (field.is_message_type || field.is_enum_type) {
                out.insert(field.resolved_type_fqn);
            }
        }
    }
    for (const auto& map : message.map_fields) {
        if (!map.resolved_value_type_fqn.empty()) {  // a message or enum map value
            out.insert(map.resolved_value_type_fqn);
        }
    }
    for (const auto& nested : message.nested_messages) {
        collect_referenced_types(nested, out);
    }
}

// Order sibling messages so a sibling enclosing a referenced type is emitted first. Only a type NESTED
// under a sibling forces ordering (it needs that sibling complete to be named); a direct sibling is
// covered by its forward declaration, cycles included. Acyclic except for inherently-uncompilable
// schemas (two siblings each naming a type nested in the other), which the active-set guard breaks.
std::vector<const MessageNode*> ordered_siblings(const std::vector<MessageNode>& siblings) {
    std::vector<std::unordered_set<std::string>> refs(siblings.size());
    for (std::size_t i = 0; i < siblings.size(); ++i) {
        collect_referenced_types(siblings[i], refs[i]);
    }
    const auto depends_on = [&](std::size_t a, std::size_t b) {  // must B precede A?
        const std::string& root =
            siblings[b].fqn;  // true iff A names a type strictly nested under B
        return std::any_of(refs[a].begin(), refs[a].end(), [&](const std::string& t) {
            return t.size() > root.size() && t.compare(0, root.size(), root) == 0 &&
                   t[root.size()] == '.';
        });
    };
    return codegen::topo_order_siblings(siblings, depends_on);
}

// Emit the struct shell: constructor, nested enums/messages, field tags, the decode() DECLARATION,
// and the byte-view member. decode() is defined out-of-line (see emit_decode_def) so that field types
// referencing other messages are complete by the time the body is compiled.
void emit_message(Printer& printer, const CppNameTable& symbols, const MessageNode& message) {
    const std::string& type = symbols.local.at(&message);
    printer.print("struct $T$ {\n", {{"T", type}});
    printer.indent();
    printer.print("explicit $T$(::rapidproto::ByteView bytes) noexcept : m_bytes(bytes) {}\n",
                  {{"T", type}});
    // The undecoded span this decoder walks: for a sub-decoder delivered to a callback, exactly
    // the sub-message's field bytes (a LEN payload, or a group/DELIMITED body without its
    // framing) -- a plain field sequence, so it feeds the ARENA model's decode() directly. That is
    // the hybrid seam: stream the outer message, materialize chosen sub-messages, without this
    // output ever depending on the arena runtime. rp_-prefixed like every generated non-field
    // identifier, so it can never collide with a field tag.
    printer.print("::rapidproto::ByteView rp_bytes() const noexcept { return m_bytes; }\n\n");

    for (const auto& nested_enum : message.enums) {
        codegen::emit_enum(printer, symbols, nested_enum, true);
    }
    // Forward-declare nested messages first: a field tag's `using Value` and any sibling cross-reference
    // (including a cycle) must name a nested type that may be defined later. Nested types can only be
    // forward-declared here, inside the enclosing struct (top-level messages get file-scope ones).
    for (const auto& nested : message.nested_messages) {
        printer.print("struct $T$;\n", {{"T", symbols.local.at(&nested)}});
    }
    for (const MessageNode* nested : ordered_siblings(message.nested_messages)) {
        emit_message(printer, symbols, *nested);
    }

    const auto fields = collect_fields(symbols, message);
    for (const auto& [field, gen] : fields) {
        printer.print(
            "struct $f$ { using Value = $vt$; static constexpr std::uint32_t kNumber = $n$;"
            " static constexpr std::string_view kName = \"$real$\"; };\n",
            {{"f", symbols.local.at(field)},
             {"vt", gen.value_type},
             {"n", std::to_string(field->number)},
             {"real", field->name}});
    }
    for (const auto& map : message.map_fields) {
        emit_map_tag(printer, symbols, map);
    }
    if (!fields.empty() || !message.map_fields.empty()) {
        printer.print("\n");
    }

    printer.print("template <class... Callbacks>\n");
    printer.print(
        "[[nodiscard]] ::rapidproto::DecodeStatus decode(Callbacks&&... rp_callbacks) const;\n");

    printer.outdent();
    printer.print(" private:\n");
    printer.indent();
    printer.print("::rapidproto::ByteView m_bytes;\n");
    printer.outdent();
    printer.print("};\n\n");
}

// Value-threaded read of one field value from the byte cursor `cur` (bounds `end`, offset base `beg`),
// advancing `cur`. Emits the read into a fresh local and returns the expression (decode_pre/post
// applied) naming the decoded value; the caller either invokes a callback with it (emit_decode_and_
// invoke) or assigns it to a target (emit_vt_read_into). On a malformed read emits a `return` of the
// wire-error DecodeStatus -- offset (cur - beg) for a leaf read (the read fails at its entry cursor,
// exactly the reader path's offset), the deep fail offset for a group. The rp_raw/rp_val/rp_np
// temporaries occupy the current block, so emit at most one read per scope (each call site is its own
// switch arm / loop body).
// NOLINTBEGIN(bugprone-easily-swappable-parameters): the cursor triple cur/end/beg are distinct
// string operands of the emitted read (a fixed, deliberate convention).
std::string emit_vt_read(Printer& printer, const FieldGen& gen, const std::string& cur,
                         const std::string& end, const std::string& beg) {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    const std::string fail =
        "if (rp_np == nullptr) { return ::rapidproto::DecodeStatus{rp_we, false,"
        " static_cast<std::size_t>(" +
        cur + " - " + beg + ")}; }\n";
    if (gen.wire_type == "Len") {
        printer.print("::rapidproto::ByteView rp_val;\n");
        printer.print(
            "const std::uint8_t* const rp_np ="
            " ::rapidproto::wire::read_length_delimited($c$, $e$, &rp_val, &rp_we);\n",
            {{"c", cur}, {"e", end}});
        printer.print(fail);
        printer.print("$c$ = rp_np;\n", {{"c", cur}});
        return gen.decode_pre + "rp_val" + gen.decode_post;
    }
    if (gen.wire_type == "SGroup") {
        // Group body up to the matching EGROUP; wire::read_group writes the deep fail offset itself.
        printer.print("::rapidproto::ByteView rp_val;\n");
        printer.print("std::size_t rp_gfo = 0;\n");
        printer.print(
            "const std::uint8_t* const rp_np = ::rapidproto::wire::read_group($c$, $e$, $b$,"
            " rp_tag.field_number, &rp_val, &rp_we, &rp_gfo);\n",
            {{"c", cur}, {"e", end}, {"b", beg}});
        printer.print(
            "if (rp_np == nullptr) { return ::rapidproto::DecodeStatus{rp_we, false, rp_gfo}; }\n");
        printer.print("$c$ = rp_np;\n", {{"c", cur}});
        return gen.decode_pre + "rp_val" + gen.decode_post;
    }
    // Varint / I32 / I64: a numeric-or-enum read into a raw integer.
    std::string vt = "read_varint";
    std::string rawty = "std::uint64_t";
    if (gen.wire_type == "I32") {
        vt = "read_fixed32";
        rawty = "std::uint32_t";
    } else if (gen.wire_type == "I64") {
        vt = "read_fixed64";
        rawty = "std::uint64_t";
    }
    printer.print("$R$ rp_raw = 0;\n", {{"R", rawty}});
    printer.print(
        "const std::uint8_t* const rp_np = ::rapidproto::wire::$vt$($c$, $e$, &rp_raw, &rp_we);\n",
        {{"vt", vt}, {"c", cur}, {"e", end}});
    printer.print(fail);
    printer.print("$c$ = rp_np;\n", {{"c", cur}});
    return gen.decode_pre + "rp_raw" + gen.decode_post;
}

// Value-threaded LEN read from the MAIN cursor rp_c into a fresh ByteView `view`, advancing rp_c.
// The former WireReader::read_length_delimited; used for map entry payloads and packed spans.
void emit_vt_len_read(Printer& printer, const std::string& view) {
    printer.print(
        "::rapidproto::ByteView $v$;\n"
        "{ const std::uint8_t* const rp_np ="
        " ::rapidproto::wire::read_length_delimited(rp_c, rp_cend, &$v$, &rp_we);"
        " if (rp_np == nullptr) { return ::rapidproto::DecodeStatus{rp_we, false,"
        " static_cast<std::size_t>(rp_c - ::rapidproto::wire::byte_ptr(m_bytes))}; } rp_c = rp_np; "
        "}\n",
        {{"v", view}});
}

// Read one value (per gen) into the lvalue `target`, threading the cursor cur/end (offset base beg).
// Used by the map key/value arms; the read temporaries live in the caller's arm scope.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): target lvalue + cursor triple, distinct roles
void emit_vt_read_into(Printer& printer, const FieldGen& gen, const std::string& target,
                       const std::string& cur, const std::string& end, const std::string& beg) {
    const std::string expr = emit_vt_read(printer, gen, cur, end, beg);
    printer.print("$t$ = $x$;\n", {{"t", target}, {"x", expr}});
}

// Emit "decode one value from the cursor cur/end (offset base beg) and invoke the callback,
// propagating errors". The cursor is the main loop's rp_c/rp_cend or a packed sub-span's cursor.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): fname + cursor triple, distinct roles
void emit_decode_and_invoke(Printer& printer, const std::string& fname, const FieldGen& gen,
                            const std::string& cur, const std::string& end,
                            const std::string& beg) {
    const std::string expr = emit_vt_read(printer, gen, cur, end, beg);
    printer.print(
        "if (const auto rp_status = ::rapidproto::invoke_field(rp_dispatch, $f${}, $x$);"
        " !rp_status.ok()) {\n",
        {{"f", fname}, {"x", expr}});
    printer.indent();
    printer.print("return rp_status;\n");
    printer.outdent();
    printer.print("}\n");
}

// Emit the body of a packed (LEN) arm for a repeated packable field: `rp_packed` is already read.
// Varint elements go through the SHARED whole-span kernel (decode_packed_varints + a callback sink,
// the same classifier/kernels the arena decoder uses); fixed32/64 stay on the per-element loop. Offsets
// are span-relative (the span start is passed as the offset base), matching the per-element path.
void emit_packed_body(Printer& printer, const std::string& fname, const FieldGen& gen) {
    emit_vt_len_read(printer, "rp_packed");
    if (gen.wire_type == "Varint") {
        printer.print(
            "const std::uint8_t* const rp_pc = ::rapidproto::wire::byte_ptr(rp_packed);\n");
        printer.print("::rapidproto::DecodeStatus rp_ab{};\n");
        printer.print("std::size_t rp_pfo = 0;\n");
        printer.print(
            "const std::size_t rp_pdc = ::rapidproto::wire::decode_packed_varints(rp_pc,"
            " rp_pc + rp_packed.size(), rp_pc, &rp_we, &rp_pfo,"
            " ::rapidproto::callback_sink<std::decay_t<decltype(rp_dispatch)>, $f$, $conv$>{"
            "&rp_dispatch, &rp_ab});\n",
            {{"f", fname}, {"conv", gen.packed_conv}});
        printer.print(
            "if (rp_pdc == static_cast<std::size_t>(-1)) { return ::rapidproto::DecodeStatus{rp_we,"
            " false, rp_pfo}; }\n");
        printer.print("if (!rp_ab.ok()) { return rp_ab; }\n");
    } else {
        // fixed32/fixed64 packed: per-element read (no varint kernel applies).
        printer.print("const std::uint8_t* rp_pc = ::rapidproto::wire::byte_ptr(rp_packed);\n");
        printer.print("const std::uint8_t* const rp_pbeg = rp_pc;\n");
        printer.print("const std::uint8_t* const rp_pe = rp_pc + rp_packed.size();\n");
        printer.print("while (rp_pc < rp_pe) {\n");
        printer.indent();
        emit_decode_and_invoke(printer, fname, gen, "rp_pc", "rp_pe", "rp_pbeg");
        printer.outdent();
        printer.print("}\n");
    }
}

void emit_arm(Printer& printer, const std::string& fname, const FieldGen& gen, bool repeated) {
    // Packable element types (numeric scalars + enums) may also arrive packed in a single LEN.
    const bool packable = codegen::is_packable_wire(gen.wire_type);

    printer.print("case $f$::kNumber:\n", {{"f", fname}});
    printer.indent();
    codegen::emit_dispatch_guards(printer, "Callbacks", fname + ", " + fname + "::Value",
                                  "field '" + fname + "'", fname + "::Value");
    printer.print(
        "if constexpr ((false || ... ||"
        " ::rapidproto::handles_one<Callbacks, $f$, $f$::Value>)) {\n",
        {{"f", fname}});
    printer.indent();

    // Native wire form: a singular field, or one element of an expanded repeated field. On a match
    // the value is consumed and we `continue` the field loop; otherwise we `break` out of the
    // switch and fall through to the single shared skip after it (see emit_decode_def).
    printer.print("if (rp_tag.wire_type == ::rapidproto::WireType::$wt$) {\n",
                  {{"wt", gen.wire_type}});
    printer.indent();
    emit_decode_and_invoke(printer, fname, gen, "rp_c", "rp_cend",
                           "::rapidproto::wire::byte_ptr(m_bytes)");
    printer.print("continue;\n");
    printer.outdent();
    printer.print("}\n");

    if (repeated && packable) {  // packed form: a LEN payload of back-to-back elements.
        printer.print("if (rp_tag.wire_type == ::rapidproto::WireType::Len) {\n");
        printer.indent();
        emit_packed_body(printer, fname, gen);
        printer.print("continue;\n");
        printer.outdent();
        printer.print("}\n");
    }

    printer.outdent();
    printer.print("}\n");
    printer.print("break;\n");  // not consumed -> shared skip after the switch
    printer.outdent();
}

// A field whose whole tag is a single byte -- number 1..15 ((15 << 3) | 7 == 127 < 128; field 16+
// needs two bytes) and not a group (a group's read needs the field number the fast path does not
// materialize). These are dispatched by the raw-byte fast-path switch; every other field stays on the
// general path. Kept as one predicate so the fast switch and the general switch partition identically.
constexpr std::int32_t kMaxOneByteTagField = 15;
bool is_fast_tag_field(const FieldNode& field, const FieldGen& gen) {
    return field.number <= kMaxOneByteTagField && gen.wire_type != "SGroup";
}

// Fast-path arm for the raw-byte tag switch (see emit_decode_def). A field whose whole tag fits in
// one byte (number 1..15) becomes a `case raw_tag(number, wire):` over WireReader::peek_byte() -- the
// wire-type check is folded into the case label, and the common tag is dispatched without splitting
// field/wire. A matched, handled field consumes the tag byte and decodes; if the field has no
// callback, `break` drops to the general path below, which skips it (so behavior is unchanged). A
// repeated packable field also gets a Len case for its packed form. Group fields are left to the
// general path (their read_group needs the field number the fast path does not materialize).
void emit_fast_arm(Printer& printer, const std::string& fname, const FieldGen& gen, bool repeated) {
    const bool packable = codegen::is_packable_wire(gen.wire_type);
    const char* const guard =
        "if constexpr ((false || ... ||"
        " ::rapidproto::handles_one<Callbacks, $f$, $f$::Value>)) {\n";

    printer.print("case ::rapidproto::raw_tag($f$::kNumber, ::rapidproto::WireType::$wt$):\n",
                  {{"f", fname}, {"wt", gen.wire_type}});
    printer.indent();
    // Same per-callback guards as the general arm, emitted before invoke_field so a callback misuse
    // (e.g. two catch-alls) reports the intended static_assert rather than a downstream ambiguity.
    codegen::emit_dispatch_guards(printer, "Callbacks", fname + ", " + fname + "::Value",
                                  "field '" + fname + "'", fname + "::Value");
    printer.print(guard, {{"f", fname}});
    printer.indent();
    printer.print("++rp_c;  // consume the peeked 1-byte tag\n");
    emit_decode_and_invoke(printer, fname, gen, "rp_c", "rp_cend",
                           "::rapidproto::wire::byte_ptr(m_bytes)");
    printer.print("continue;\n");
    printer.outdent();
    printer.print("}\n");
    printer.print("break;\n");  // no callback -> fall to the general path, which skips it
    printer.outdent();

    if (repeated && packable) {  // packed form: a LEN payload of back-to-back elements.
        printer.print("case ::rapidproto::raw_tag($f$::kNumber, ::rapidproto::WireType::Len):\n",
                      {{"f", fname}});
        printer.indent();
        printer.print(guard, {{"f", fname}});
        printer.indent();
        printer.print("++rp_c;  // consume the peeked 1-byte tag\n");
        emit_packed_body(printer, fname, gen);
        printer.print("continue;\n");
        printer.outdent();
        printer.print("}\n");
        printer.print("break;\n");
        printer.outdent();
    }
}

// Out-of-line decode() definition for `message` (whose C++ name, qualified within the namespace, is
// `qualifier`), plus its nested messages. Emitted after all struct shells so every field type is a
// complete type here (handles forward and cyclic message references).
void emit_decode_def(Printer& printer, const CppNameTable& symbols, const MessageNode& message,
                     const std::string& qualifier) {
    const auto fields = collect_fields(symbols, message);
    printer.print("template <class... Callbacks>\n");
    // RP_FLATTEN: inline the wire primitives / dispatch / sub-decodes into this one function. GCC's
    // large-TU inliner is otherwise far more conservative than Clang's, leaving them out-of-line.
    printer.print(
        "RP_FLATTEN ::rapidproto::DecodeStatus $Q$::decode(Callbacks&&... rp_callbacks) const {\n",
        {{"Q", qualifier}});
    printer.indent();
    // Per-callback stray guard: every callback must name one of THIS message's tags (or be a
    // catch-all / unknown-field handler). Catches a callback pasted from another message's
    // decode(), which no per-field guard would ever see.
    std::string tags;
    for (const auto& [field, gen] : fields) {
        tags += ", " + symbols.local.at(field);
    }
    for (const auto& map : message.map_fields) {
        tags += ", " + symbols.local.at(&map);
    }
    printer.print(
        "static_assert((true && ... && !::rapidproto::is_stray_callback<Callbacks$tags$>),"
        " \"a callback matches no field of '$Q$' (and is not a catch-all or unknown-field"
        " handler)\");\n",
        {{"tags", tags}, {"Q", qualifier}});
    printer.print(
        "[[maybe_unused]] auto rp_dispatch = "
        "::rapidproto::combine(static_cast<Callbacks&&>(rp_callbacks)...);\n");
    // Value-threaded wire loop: the cursor (rp_c) is threaded by value through the rapidproto::wire:: reader/skip free
    // functions and stays in registers -- no WireReader member whose address escapes to memory. Fail
    // offsets are anchored at byte_ptr(m_bytes); rp_we is the shared error slot used by every arm.
    printer.print("const std::uint8_t* rp_c = ::rapidproto::wire::byte_ptr(m_bytes);\n");
    printer.print("const std::uint8_t* const rp_cend = rp_c + m_bytes.size();\n");
    printer.print("::rapidproto::Tag rp_tag{};\n");
    printer.print("::rapidproto::WireError rp_we = ::rapidproto::WireError::None;\n");
    printer.print("for (;;) {\n");
    printer.indent();
    // Fast path: a raw-byte switch dispatches single-byte-tag fields (is_fast_tag_field) without
    // splitting field/wire (see emit_fast_arm). A miss (multi-byte tag, unknown field, or wrong wire
    // type) breaks to the general path below. Each fast field's decode body is emitted ONLY here; the
    // general switch keeps just a trivial `case N: break;` for it, so a wrong-wire encoding of a fast
    // field skips exactly as a >15 field would (consistent) without duplicating the body.
    std::vector<std::pair<const FieldNode*, FieldGen>> fast;
    for (const auto& [field, gen] : fields) {
        if (is_fast_tag_field(*field, gen)) {
            fast.emplace_back(field, gen);
        }
    }
    if (!fast.empty()) {
        printer.print("if (rp_c >= rp_cend) { return ::rapidproto::DecodeStatus::success(); }\n");
        printer.print("switch (*rp_c) {  // peek the 1-byte tag without consuming\n");
        printer.indent();
        for (const auto& [field, gen] : fast) {
            emit_fast_arm(printer, symbols.local.at(field), gen, field->is_repeated);
        }
        printer.print("default: break;\n");
        printer.outdent();
        printer.print("}\n");
    }
    // General path: multi-byte tags, unknown fields, wrong wire types, groups, maps.
    // Fused end-or-tag read: one bounds check drives the loop (see WireReader::read_tag_or_end).
    printer.print("::rapidproto::wire::TagState rp_state = ::rapidproto::wire::TagState::End;\n");
    printer.print(
        "const std::uint8_t* const rp_tp ="
        " ::rapidproto::wire::read_tag_or_end(rp_c, rp_cend, &rp_tag, &rp_we, &rp_state);\n");
    printer.print(
        "if (rp_state == ::rapidproto::wire::TagState::End) {"
        " return ::rapidproto::DecodeStatus::success(); }\n");
    printer.print(
        "if (rp_state == ::rapidproto::wire::TagState::Error) { return "
        "::rapidproto::DecodeStatus{rp_we,"
        " false, static_cast<std::size_t>(rp_c - ::rapidproto::wire::byte_ptr(m_bytes))}; }\n");
    printer.print("rp_c = rp_tp;\n");
    printer.print("switch (rp_tag.field_number) {\n");
    printer.indent();
    for (const auto& [field, gen] : fields) {
        if (is_fast_tag_field(*field, gen)) {
            // The decode body lives in the fast switch; this bare case only SKIPS a malformed
            // encoding of the field that missed the fast path -- a wrong wire type, or a
            // non-canonical (over-long) tag. A canonical 1-byte tag always hits the fast path, so
            // only malformed input reaches here (protoc emits neither); skipping it never crashes,
            // and its output is unspecified anyway. (No body here -> the fast field's decode is
            // emitted once.)
            printer.print("case $f$::kNumber: break;\n", {{"f", symbols.local.at(field)}});
        } else {
            emit_arm(printer, symbols.local.at(field), gen, field->is_repeated);
        }
    }
    for (const auto& map : message.map_fields) {
        emit_map_arm(printer, symbols, map);
    }
    // `default` = a field number not in the schema. If the caller passed a callback that
    // specifically handles UnknownField, deliver the raw field to it; otherwise (including when
    // only a generic field catch-all is present) fall through to the shared skip below.
    printer.print("default:\n");
    printer.indent();
    printer.print(
        "if constexpr ((false || ... ||"
        " ::rapidproto::specifically_handles_unknown<Callbacks>)) {\n");
    printer.indent();
    printer.print(
        "const std::size_t rp_value_start ="
        " static_cast<std::size_t>(rp_c - ::rapidproto::wire::byte_ptr(m_bytes));\n");
    printer.print("std::size_t rp_ufo = 0;\n");
    printer.print(
        "const std::uint8_t* const rp_usp = ::rapidproto::wire::skip_value(rp_c, rp_cend,"
        " ::rapidproto::wire::byte_ptr(m_bytes), rp_tag, 0, &rp_we, &rp_ufo);\n");
    printer.print(
        "if (rp_usp == nullptr) { return ::rapidproto::DecodeStatus{rp_we, false, rp_ufo}; }\n");
    printer.print("rp_c = rp_usp;\n");
    printer.print(
        "if (const auto rp_status = ::rapidproto::invoke_unknown(rp_dispatch,"
        " ::rapidproto::UnknownField{rp_tag.field_number, rp_tag.wire_type,"
        " m_bytes.substr(rp_value_start,"
        " static_cast<std::size_t>(rp_c - ::rapidproto::wire::byte_ptr(m_bytes)) - "
        "rp_value_start)});"
        " !rp_status.ok()) {\n");
    printer.indent();
    printer.print("return rp_status;\n");
    printer.outdent();
    printer.print("}\n");
    printer.print("continue;\n");
    printer.outdent();
    printer.print("}\n");  // if constexpr handles_unknown
    printer.print("break;\n");
    printer.outdent();
    printer.outdent();
    printer.print("}\n");  // switch
    // A field that wasn't consumed by a case (a known field with no callback or a non-matching wire
    // type, or an unknown field with no unknown-handler) is skipped here -- one shared skip site.
    printer.print("std::size_t rp_fo = 0;\n");
    printer.print(
        "const std::uint8_t* const rp_sp = ::rapidproto::wire::skip_value(rp_c, rp_cend,"
        " ::rapidproto::wire::byte_ptr(m_bytes), rp_tag, 0, &rp_we, &rp_fo);\n");
    printer.print(
        "if (rp_sp == nullptr) { return ::rapidproto::DecodeStatus{rp_we, false, rp_fo}; }\n");
    printer.print("rp_c = rp_sp;\n");
    printer.outdent();
    printer.print("}\n");  // for (;;) -- exits only via return (End / Error / a field abort)
    printer.outdent();
    printer.print("}\n\n");

    for (const auto& nested : message.nested_messages) {
        emit_decode_def(printer, symbols, nested, qualifier + "::" + symbols.local.at(&nested));
    }
}

// An import path -> the generated header it produces: "foo/bar.proto" -> "foo/bar.rp.stream.hpp".
std::string import_header(std::string_view path) {
    return codegen::import_header(path, ".rp.stream.hpp");
}

}  // namespace

std::string generate_header(const FileNode& file, const CppNameTable& symbols) {
    Printer printer;
    printer.print("// Generated by rapidprotoc $v$. DO NOT EDIT.\n", {{"v", kVersion}});
    printer.print(
        "// Generated from your schema; depends on rapidproto/runtime.hpp (Apache-2.0).\n");
    printer.print("#pragma once\n\n");
    printer.print("#include <cstdint>\n");
    printer.print("#include <string_view>\n\n");
    printer.print("#include \"rapidproto/runtime.hpp\"\n");
    // The schema's top-level enums live in the shared common header (one C++ type, shared with the
    // arena decoder); include this file's own sibling common. The IWYU export makes a TU that includes
    // only this decoder still "directly provide" the shared enums (which used to live here).
    printer.print("#include \"$c$\"  // IWYU pragma: export\n",
                  {{"c", codegen::common_sibling_include(file.filename)}});
    // A field whose type comes from an imported .proto references that import's generated header;
    // emit an include per import so the output compiles standalone. Public imports re-export their
    // own imports, so the chain stays closed by including every direct import. The resolver makes
    // every import's symbols visible as field types EXCEPT option-only imports, so we mirror that:
    // include all but `Option` (a weak import's type is still usable, so it still needs its
    // header).
    for (const auto& import : file.imports) {
        if (import.kind != ImportKind::Option) {
            printer.print("#include \"$h$\"\n", {{"h", import_header(import.path)}});
        }
    }
    printer.print("\n");

    const std::string ns = codegen::message_namespace(symbols, file);
    if (!ns.empty()) {
        printer.print("namespace $ns$ {\n\n", {{"ns", ns}});
    }

    // Top-level enums live in the shared common header (one C++ type, shared with the arena decoder);
    // alias each into this model namespace so `<pkg>::stream::Enum` resolves. Nested enums ride inside
    // their message.
    for (const auto& node : file.enums) {
        printer.print("using $e$;\n", {{"e", cpp_type_name(symbols, node.fqn)}});
    }
    if (!file.enums.empty()) {
        printer.print("\n");
    }
    for (const auto& message : file.messages) {  // forward declarations (cross-references)
        printer.print("struct $T$;\n", {{"T", symbols.local.at(&message)}});
    }
    if (!file.messages.empty()) {
        printer.print("\n");
    }
    for (const MessageNode* message :
         ordered_siblings(file.messages)) {  // shells, in nested-type-reference order
        emit_message(printer, symbols, *message);
    }
    for (const auto& message :
         file.messages) {  // out-of-line decode() definitions (types complete)
        emit_decode_def(printer, symbols, message, symbols.local.at(&message));
    }

    if (!ns.empty()) {
        printer.print("}  // namespace $ns$\n", {{"ns", ns}});
    }
    return printer.str();
}

std::string generate_header(const FileNode& file, const std::vector<FileNode>& all_files,
                            const std::string& namespace_prefix) {
    // Convenience: build the name table here. A caller emitting a whole resolved set should instead
    // build it ONCE with build_cpp_names and call the (file, names) overload per file. The prefix
    // is dot-separated (proto convention); namespace_of sanitizes + ::-joins it.
    return generate_header(
        file, build_cpp_names(file, all_files, namespace_of(namespace_prefix), "stream"));
}

std::string generate_header(const FileNode& file) {
    return generate_header(
        file, build_cpp_names(file, {}, std::string{}, "stream"));  // single-file table
}

}  // namespace rapidproto::streamgen
