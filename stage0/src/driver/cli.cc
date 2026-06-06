#include "cli.h"

#include <cstdio>
#include <cstring>
#include <string>

#include "build.h"

namespace {

void print_usage() {
    std::printf(
        "digon stage 0 (v0.1 pre-alpha)\n"
        "\n"
        "usage: digon <command> [args]\n"
        "\n"
        "commands:\n"
        "  build       compile current package\n"
        "  run         compile and run current package\n"
        "  check       type-check only\n"
        "  test        run @test functions\n"
        "  fmt         format sources\n"
        "  new <name>  scaffold a new package\n"
        "  init        initialise package in cwd\n"
        "  lsp         language server\n"
        "  version     print version and LLVM info\n"
        "  help        print this message\n");
}

int cmd_unimplemented(const char* name) {
    std::fprintf(stderr, "digon: command '%s' is not implemented yet\n", name);
    return 1;
}

int cmd_version() {
    std::printf("digon stage 0 v0.1 (pre-alpha)\n");
    return 0;
}

// Scan a subcommand's args (argv[2..]) for a single source file, a --release
// flag, and an optional `-o <out>`. Returns the file or nullptr.
const char* parse_build_args(int argc, char** argv, bool* release, const char** out) {
    const char* file = nullptr;
    for (int i = 2; i < argc; i++) {
        if (std::strcmp(argv[i], "--release") == 0) {
            *release = true;
        } else if (std::strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            *out = argv[++i];
        } else if (argv[i][0] != '-') {
            file = argv[i];
        }
    }
    return file;
}

int cmd_build(int argc, char** argv) {
    bool        release = false;
    const char* out     = nullptr;
    const char* file    = parse_build_args(argc, argv, &release, &out);
    if (!file) {
        std::fprintf(stderr, "digon build: expected a source file\n");
        return 1;
    }
    std::string derived;
    if (!out) {
        derived = file;
        size_t dot = derived.find_last_of('.');
        size_t sep = derived.find_last_of("/\\");
        if (dot != std::string::npos && (sep == std::string::npos || dot > sep)) derived.erase(dot);
        derived += ".exe";
        out = derived.c_str();
    }
    return build_file(file, out, release);
}

int cmd_run(int argc, char** argv) {
    bool        release = false;
    const char* out     = nullptr;
    const char* file    = parse_build_args(argc, argv, &release, &out);
    if (!file) {
        std::fprintf(stderr, "digon run: expected a source file\n");
        return 1;
    }
    return run_file(file, release);
}

int cmd_fmt(int argc, char** argv) {
    const char* file = nullptr;
    for (int i = 2; i < argc; i++)
        if (argv[i][0] != '-') file = argv[i];
    if (!file) {
        std::fprintf(stderr, "digon fmt: expected a source file\n");
        return 1;
    }
    return format_file(file);
}

} // namespace

int cli_dispatch(int argc, char** argv) {
    if (argc < 2) {
        print_usage();
        return 0;
    }
    const char* cmd = argv[1];

    if (std::strcmp(cmd, "help") == 0 || std::strcmp(cmd, "-h") == 0 ||
        std::strcmp(cmd, "--help") == 0) {
        print_usage();
        return 0;
    }
    if (std::strcmp(cmd, "version") == 0 || std::strcmp(cmd, "--version") == 0) {
        return cmd_version();
    }

    if (std::strcmp(cmd, "build") == 0) return cmd_build(argc, argv);
    if (std::strcmp(cmd, "run")   == 0) return cmd_run(argc, argv);
    if (std::strcmp(cmd, "check") == 0) return cmd_unimplemented("check");
    if (std::strcmp(cmd, "test")  == 0) return cmd_unimplemented("test");
    if (std::strcmp(cmd, "fmt")   == 0) return cmd_fmt(argc, argv);
    if (std::strcmp(cmd, "new")   == 0) return cmd_unimplemented("new");
    if (std::strcmp(cmd, "init")  == 0) return cmd_unimplemented("init");
    if (std::strcmp(cmd, "lsp")   == 0) return cmd_unimplemented("lsp");

    std::fprintf(stderr, "digon: unknown command '%s'\n", cmd);
    print_usage();
    return 2;
}
