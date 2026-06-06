# Contributing to digon

Thanks for your interest in improving digon. This guide covers how to build,
test, and submit changes.

## Building and testing

The bootstrap compiler lives in `stage0/`. You need a C++17 compiler,
CMake 3.20+, and LLVM 21 + LLD 21.

```sh
cd stage0
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

Please make sure the full suite passes before opening a pull request, and add a
golden test (`stage0/tests/golden/`) or compile-fail test
(`stage0/tests/compile_fail/`) covering any behavior change.

## Submitting changes

1. Fork the repository and create a topic branch.
2. Keep commits focused and write clear commit messages.
3. Open a pull request describing what changed and why.

## Developer Certificate of Origin (DCO)

digon tracks contribution provenance with the
[Developer Certificate of Origin](https://developercertificate.org/) instead of
a CLA. By signing off on a commit you certify that you wrote the change (or
otherwise have the right to submit it) and that it may be distributed under the
project's license.

Sign off every commit:

```sh
git commit -s -m "Your message"
```

This appends a line that must match your real identity:

```
Signed-off-by: Your Name <your.email@example.com>
```

## Licensing of contributions

By contributing, you agree that your contributions are licensed under the
project's terms — **MIT OR Apache-2.0**, at the user's option. You keep
copyright to what you write; the DCO sign-off records that it is submitted under
these terms. See [LICENSE-MIT](LICENSE-MIT) and [LICENSE-APACHE](LICENSE-APACHE).
