# digon

A small, fast-compiling systems language with compile-time memory safety,
arena-first data, structural traits, and native C interop. Compiles to native
executables via LLVM.

> Pre-alpha. The language and toolchain are under active development.

## Build

Requires a C++17 compiler, CMake 3.20+, **LLVM 21** and **LLD 21**, and a C
compiler (`cc`) on `PATH`.

```sh
cd stage0
cmake -S . -B build -G Ninja
cmake --build build
```

## Test

```sh
ctest --test-dir stage0/build --output-on-failure
```

## Usage

```sh
digon build program.dg   # compile to a native executable
digon run   program.dg   # compile and run
digon fmt   program.dg   # print canonical formatting
```

## Example

```digon
type Point { x: i64, y: i64 }

func area(p: ref Point) -> i64 { p.x * p.y }

func main() -> i32 {
    let p = Point { x: 3, y: 4 }
    p.area() as i32
}
```

## License

Licensed under either of

- Apache License, Version 2.0 ([LICENSE-APACHE](LICENSE-APACHE) or
  <http://www.apache.org/licenses/LICENSE-2.0>)
- MIT license ([LICENSE-MIT](LICENSE-MIT) or
  <http://opensource.org/licenses/MIT>)

at your option.

### Contribution

Unless you explicitly state otherwise, any contribution intentionally submitted
for inclusion in the work by you, as defined in the Apache-2.0 license, shall be
dual licensed as above, without any additional terms or conditions.
