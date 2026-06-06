#ifndef DIGON_LINK_H
#define DIGON_LINK_H

#include "diag.h"

// Link a single object file into a native executable by invoking the system C
// compiler driver (cc), which knows where the CRT, startup objects, and
// default libraries live. The driver can be overridden via $DIGON_CC.
// Returns true on success; reports failures through `diag`.
bool link_executable(const char* obj_path, const char* exe_path, Diag* diag);

#endif
