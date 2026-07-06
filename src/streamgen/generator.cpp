#include "rapidproto/streamgen/generator.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
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

// How one field maps to generated code: the C++ Value delivered to the callback, the wire type
// its arm matches, the reader method to call, and the expression turning the raw read value
// (`*value`) into the Value (decode_pre + "*value" + decode_post).
struct FieldGen {
    std::string value_type;
    std::string_view wire_type;  // WireType enumerator
    std::string_view read_call;  // WireReader method
    std::string decode_pre;      // wraps *value -> Value
    std::string decode_post;
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
    return FieldGen{std::move(value_type), w->wire, w->read, std::string(w->pre),
                    std::string(w->post)};
}

// How to decode one scalar / enum / message / group field (the Value the callback receives).
FieldGen field_gen(const CppNameTable& symbols, const FieldNode& field) {
    if (std::optional<FieldGen> scalar = scalar_gen(field.type_name)) {
        return *scalar;
    }
    const std::string cpp = cpp_type_name(symbols, field.resolved_type_fqn);
    if (field.is_enum_type) {
        return {cpp, "Varint", "read_varint()",
                "static_cast<" + cpp + ">(::rapidproto::varint_to_int32(", "))"};
    }
    if (field.message_encoding == MessageEncoding::Delimited) {
        // Delimited wire format (a proto2 `group` or an editions DELIMITED message field): the
        // Value is the sub-decoder over the body delimited by SGROUP/EGROUP. (`is_group` only marks
        // the synthesized-nested-message structure; the wire form is decided by
        // `message_encoding`.)
        return {cpp, "SGroup", "read_group(rp_tag.field_number)", cpp + "{", "}"};
    }
    // message-typed: the Value is the sub-decoder, constructed over the LEN payload.
    return {cpp, "Len", "read_length_delimited()", cpp + "{", "}"};
}

// How to decode a map field's value (scalar / enum / message). The key is a scalar (analyze checks).
FieldGen map_value_gen(const CppNameTable& symbols, const MapFieldNode& map) {
    if (std::optional<FieldGen> scalar = scalar_gen(map.value_type)) {
        return *scalar;
    }
    const std::string cpp = cpp_type_name(symbols, map.resolved_value_type_fqn);
    if (map.value_is_enum) {
        return {cpp, "Varint", "read_varint()",
                "static_cast<" + cpp + ">(::rapidproto::varint_to_int32(", "))"};
    }
    return {cpp, "Len", "read_length_delimited()", cpp + "{", "}"};  // message value
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
    printer.print("const auto rp_entry = rp_reader.read_length_delimited();\n");
    printer.print(
        "if (!rp_entry) { return ::rapidproto::DecodeStatus::from_reader(rp_reader); }\n");
    printer.print("$f$::Key rp_key{};\n", {{"f", fname}});
    printer.print("$f$::Value rp_value{$d$};\n", {{"f", fname}, {"d", value_default}});
    printer.print("::rapidproto::WireReader rp_entry_reader{*rp_entry};\n");
    printer.print("::rapidproto::Tag rp_et;\n");
    printer.print("for (;;) {\n");
    printer.indent();
    printer.print("const auto rp_es = rp_entry_reader.read_tag_or_end(rp_et);\n");
    printer.print("if (rp_es == ::rapidproto::WireReader::TagOrEnd::End) { break; }\n");
    printer.print(
        "if (rp_es == ::rapidproto::WireReader::TagOrEnd::Error) {"
        " return ::rapidproto::DecodeStatus::from_reader(rp_entry_reader); }\n");
    printer.print(
        "if (rp_et.field_number == 1 && rp_et.wire_type == ::rapidproto::WireType::$kw$) {\n",
        {{"kw", key.wire_type}});
    printer.indent();
    printer.print("const auto rp_v = rp_entry_reader.$kr$;\n", {{"kr", key.read_call}});
    printer.print(
        "if (!rp_v) { return ::rapidproto::DecodeStatus::from_reader(rp_entry_reader); }\n");
    printer.print("rp_key = $pre$*rp_v$post$;\n",
                  {{"pre", key.decode_pre}, {"post", key.decode_post}});
    printer.outdent();
    printer.print(
        "} else if (rp_et.field_number == 2 &&"
        " rp_et.wire_type == ::rapidproto::WireType::$vw$) {\n",
        {{"vw", value.wire_type}});
    printer.indent();
    printer.print("const auto rp_v = rp_entry_reader.$vr$;\n", {{"vr", value.read_call}});
    printer.print(
        "if (!rp_v) { return ::rapidproto::DecodeStatus::from_reader(rp_entry_reader); }\n");
    printer.print("rp_value = $pre$*rp_v$post$;\n",
                  {{"pre", value.decode_pre}, {"post", value.decode_post}});
    printer.outdent();
    printer.print("} else if (!rp_entry_reader.skip(rp_et.wire_type, rp_et.field_number)) {\n");
    printer.indent();
    printer.print("return ::rapidproto::DecodeStatus::from_reader(rp_entry_reader);\n");
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

// Emit "decode one value from $src$ and invoke the callback, propagating errors". $src$ is the
// reader the element is read from (the message reader, or a sub-reader over a packed payload).
void emit_decode_and_invoke(Printer& printer, const std::string& fname, const FieldGen& gen,
                            std::string_view src) {
    printer.print("const auto rp_value = $src$.$read$;\n", {{"src", src}, {"read", gen.read_call}});
    printer.print("if (!rp_value) { return ::rapidproto::DecodeStatus::from_reader($src$); }\n",
                  {{"src", src}});
    printer.print(
        "if (const auto rp_status = ::rapidproto::invoke_field(rp_dispatch, $f${}, "
        "$pre$*rp_value$post$);"
        " !rp_status.ok()) {\n",
        {{"f", fname}, {"pre", gen.decode_pre}, {"post", gen.decode_post}});
    printer.indent();
    printer.print("return rp_status;\n");
    printer.outdent();
    printer.print("}\n");
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
    emit_decode_and_invoke(printer, fname, gen, "rp_reader");
    printer.print("continue;\n");
    printer.outdent();
    printer.print("}\n");

    if (repeated && packable) {  // packed form: a LEN payload of back-to-back elements.
        printer.print("if (rp_tag.wire_type == ::rapidproto::WireType::Len) {\n");
        printer.indent();
        printer.print("const auto rp_packed = rp_reader.read_length_delimited();\n");
        printer.print(
            "if (!rp_packed) { return ::rapidproto::DecodeStatus::from_reader(rp_reader); }\n");
        printer.print("::rapidproto::WireReader rp_elements{*rp_packed};\n");
        printer.print("while (!rp_elements.at_end()) {\n");
        printer.indent();
        emit_decode_and_invoke(printer, fname, gen, "rp_elements");
        printer.outdent();
        printer.print("}\n");
        printer.print("continue;\n");
        printer.outdent();
        printer.print("}\n");
    }

    printer.outdent();
    printer.print("}\n");
    printer.print("break;\n");  // not consumed -> shared skip after the switch
    printer.outdent();
}

// Out-of-line decode() definition for `message` (whose C++ name, qualified within the namespace, is
// `qualifier`), plus its nested messages. Emitted after all struct shells so every field type is a
// complete type here (handles forward and cyclic message references).
void emit_decode_def(Printer& printer, const CppNameTable& symbols, const MessageNode& message,
                     const std::string& qualifier) {
    const auto fields = collect_fields(symbols, message);
    printer.print("template <class... Callbacks>\n");
    printer.print("::rapidproto::DecodeStatus $Q$::decode(Callbacks&&... rp_callbacks) const {\n",
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
    printer.print("::rapidproto::WireReader rp_reader{m_bytes};\n");
    printer.print("::rapidproto::Tag rp_tag;\n");
    printer.print("for (;;) {\n");
    printer.indent();
    // Fused end-or-tag read: one bounds check drives the loop (see WireReader::read_tag_or_end).
    printer.print("const auto rp_state = rp_reader.read_tag_or_end(rp_tag);\n");
    printer.print(
        "if (rp_state == ::rapidproto::WireReader::TagOrEnd::End) {"
        " return ::rapidproto::DecodeStatus::success(); }\n");
    printer.print(
        "if (rp_state == ::rapidproto::WireReader::TagOrEnd::Error) {"
        " return ::rapidproto::DecodeStatus::from_reader(rp_reader); }\n");
    printer.print("switch (rp_tag.field_number) {\n");
    printer.indent();
    for (const auto& [field, gen] : fields) {
        emit_arm(printer, symbols.local.at(field), gen, field->is_repeated);
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
    printer.print("const auto rp_value_start = rp_reader.position();\n");
    printer.print(
        "if (!rp_reader.skip(rp_tag.wire_type, rp_tag.field_number)) {"
        " return ::rapidproto::DecodeStatus::from_reader(rp_reader); }\n");
    printer.print(
        "if (const auto rp_status = ::rapidproto::invoke_unknown(rp_dispatch,"
        " ::rapidproto::UnknownField{rp_tag.field_number, rp_tag.wire_type,"
        " m_bytes.substr(rp_value_start, rp_reader.position() - rp_value_start)});"
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
    printer.print(
        "if (!rp_reader.skip(rp_tag.wire_type, rp_tag.field_number)) {"
        " return ::rapidproto::DecodeStatus::from_reader(rp_reader); }\n");
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
