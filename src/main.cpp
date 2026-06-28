// rapidproto CLI: resolve a .proto entry file and its imports, run the full semantic
// pipeline (features -> FQNs -> type resolution -> option interpretation), and print a
// summary. A thin driver over the library; not part of the linted library surface.

#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "rapidproto/resolve.hpp"
#include "rapidproto/resolver.hpp"
#include "rapidproto/result.hpp"

namespace {

void print_usage(std::string_view prog) {
    std::cerr << "usage: " << prog << " [-I <include_path>]... [--no-wellknown] <entry.proto>\n";
}

}  // namespace

int main(int argc, char** argv) {
    rapidproto::ResolverConfig config;
    std::string entry;

    const std::vector<std::string> args(argv + 1, argv + argc);
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "-I") {
            if (++i >= args.size()) {
                print_usage(argv[0]);
                return 2;
            }
            config.include_paths.push_back(args[i]);
        } else if (arg.rfind("-I", 0) == 0) {
            config.include_paths.push_back(arg.substr(2));
        } else if (arg == "--no-wellknown") {
            config.use_wellknown = false;
        } else if (entry.empty()) {
            entry = arg;
        } else {
            print_usage(argv[0]);
            return 2;
        }
    }

    if (entry.empty()) {
        print_usage(argv[0]);
        return 2;
    }

    rapidproto::SourceRegistry sources;
    auto resolved = rapidproto::resolve(entry, config, sources);
    if (resolved.is_err()) {
        std::cerr << "error: " << rapidproto::render_error(resolved.error(), sources) << '\n';
        return 1;
    }
    rapidproto::ResolvedFileSet set = std::move(resolved).value();

    auto analyzed = rapidproto::analyze(set);  // features -> FQNs -> types -> options
    if (analyzed.is_err()) {
        std::cerr << "error: " << rapidproto::render_error(analyzed.error(), sources) << '\n';
        return 1;
    }

    std::cout << "resolved " << set.files.size() << " file(s) in topological order; "
              << analyzed.value().symbols.size() << " type(s), "
              << analyzed.value().extensions.size() << " extension(s):\n";
    for (const auto& file : set.files) {
        std::cout << "  " << file.filename << "  (" << file.messages.size() << " message(s), "
                  << file.enums.size() << " enum(s), " << file.imports.size() << " import(s))\n";
    }
    return 0;
}
