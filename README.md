<p align="center">
  <img src="branding/digon-logo-globe.svg" alt="digon logo" width="170" height="170">
</p>

<h1 align="center">digon</h1>

<p align="center">
  A small, fast-compiling systems language with compile-time memory safety,<br>
  arena-first data, structural traits, and native C interop.
</p>

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

- [LICENSE-APACHE-2.0](LICENSE-APACHE)
- [LICENSE-MIT](LICENSE-MIT)

at your option.

### Contribution

Unless you explicitly state otherwise, any contribution intentionally submitted
for inclusion in the work by you, as defined in the Apache-2.0 license, shall be
dual licensed as above, without any additional terms or conditions.
