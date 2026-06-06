#include "link.h"

#include <cstdlib>
#include <string>

namespace {

void append_quoted(std::string& cmd, const char* s) {
    cmd += '"';
    cmd += s;
    cmd += '"';
}

} // namespace

bool link_executable(const char* obj_path, const char* exe_path, Diag* diag) {
    const char* cc = std::getenv("DIGON_CC");
    if (!cc || !*cc) cc = "cc";

    std::string cmd = cc;
    cmd += ' ';
    append_quoted(cmd, obj_path);
    cmd += " -o ";
    append_quoted(cmd, exe_path);

    int rc = std::system(cmd.c_str());
    if (rc != 0) {
        diag_errorf(diag, Span{0, 0, 0}, "linker '%s' failed (exit %d)", cc, rc);
        return false;
    }
    return true;
}
