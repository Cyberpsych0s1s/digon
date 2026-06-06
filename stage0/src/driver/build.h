#ifndef DIGON_BUILD_H
#define DIGON_BUILD_H

// Compile `src_path` to a native executable at `out_exe`.
// Returns a process exit code: 0 = success, 1 = user/compile error,
// 2 = internal error.
int build_file(const char* src_path, const char* out_exe, bool release);

// Build `src_path` to a temporary executable, run it, and return the program's
// exit code (or a build error code if compilation failed).
int run_file(const char* src_path, bool release);

// Parse `src_path` and print its canonical formatting to stdout. Returns 0 on
// success, 1 if the file cannot be read or has syntax errors.
int format_file(const char* src_path);

#endif
