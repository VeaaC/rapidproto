#include "rapidproto/interpret.hpp"

#include <optional>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "rapidproto/ast.hpp"
#include "rapidproto/resolver.hpp"
#include "rapidproto/result.hpp"

namespace rapidproto {
namespace {

// The value of a simple (1-component, non-extension) option named `name`, or null.
const OptionValue* find_option(const std::vector<Option>& options, std::string_view name) {
    for (const auto& option : options) {
        if (option.name.size() == 1 && !option.name[0].is_extension &&
            option.name[0].name == name) {
            return &option.value;
        }
    }
    return nullptr;
}

void interpret_field(FieldNode& field) {
    // [packed = true/false] overrides the syntax-default repeated encoding, on packable scalars
    // only (matching the feature pass). The is_message/is_enum guard requires the type-resolution
    // pass to have run first, so this never clobbers its enum/message -> Expanded fixup.
    // true/false are retained as identifiers since the parser does not interpret bools.
    if (field.is_repeated && !field.is_message_type && !field.is_enum_type) {
        if (const OptionValue* packed = find_option(field.options, "packed")) {
            if (const auto* value = std::get_if<Identifier>(&packed->value)) {
                if (value->name == "true") {
                    field.repeated_encoding = RepeatedEncoding::Packed;
                } else if (value->name == "false") {
                    field.repeated_encoding = RepeatedEncoding::Expanded;
                }
            }
        }
    }
    // proto2 [default = X] -> typed default_value (kept as the parsed text-format value).
    if (const OptionValue* value = find_option(field.options, "default")) {
        field.default_value = *value;
    }
}

std::optional<Error> check_message_set_wire_format(const MessageNode& message) {
    const OptionValue* option = find_option(message.options, "message_set_wire_format");
    if (option == nullptr) {
        return std::nullopt;
    }
    const auto* value = std::get_if<Identifier>(&option->value);
    if (value != nullptr && value->name == "true") {
        return Error{0, "message-set wire format is not supported: " + message.fqn};
    }
    return std::nullopt;
}

std::optional<Error> interpret_message(MessageNode& message) {
    if (auto error = check_message_set_wire_format(message)) {
        return error;
    }
    for (auto& field : message.fields) {
        interpret_field(field);
    }
    for (auto& oneof : message.oneofs) {
        for (auto& field : oneof.fields) {
            interpret_field(field);
        }
    }
    for (auto& extend : message.extends) {
        for (auto& field : extend.fields) {
            interpret_field(field);
        }
    }
    for (auto& nested : message.nested_messages) {
        if (auto error = interpret_message(nested)) {
            return error;
        }
    }
    return std::nullopt;
}

}  // namespace

Result<std::monostate> interpret_options(FileNode& file) {
    for (auto& message : file.messages) {
        if (auto error = interpret_message(message)) {
            return *error;
        }
    }
    for (auto& extend : file.extends) {
        for (auto& field : extend.fields) {
            interpret_field(field);
        }
    }
    return std::monostate{};
}

Result<std::monostate> interpret_options(ResolvedFileSet& file_set) {
    for (auto& file : file_set.files) {
        if (auto result = interpret_options(file); result.is_err()) {
            return std::move(result).error();
        }
    }
    return std::monostate{};
}

}  // namespace rapidproto
