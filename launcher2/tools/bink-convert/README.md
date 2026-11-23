# WWE 2K14 Bink converter

`wwe2k14-bink-convert IN.bik2 OUT.bik2 [--progress]` converts a WWE '13 `KB2d` or WWE 2K15 `KB2i` titantron to a WWE 2K14 `BIKi` movie. It runs as a separate command-line program. The launcher can pass file paths directly; on Windows it starts the converter with wide-character process and file APIs, so paths containing non-ASCII characters are supported.

Build from this source directory with Rust 1.69 or later:

```sh
cargo build --release --locked --offline
```

The converter has no registry dependencies. The NihAV decoder crates it needs are included under `vendor/nihav`; the build does not fetch code from the network. To use one encoder thread, set `WWE_BINK_THREADS=1`. The default uses the available processor count, up to 64 threads. This setting does not change the output bytes.

With `--progress`, the program writes `PROGRESS<TAB>done<TAB>total<TAB>percent` lines to standard output. Errors go to standard error and begin with `ERROR<TAB>`. It writes to `OUT.bik2.part` while converting and replaces the requested output after the full movie is ready.

The output uses the existing 106-video BIKi settings (quantiser 13, skip threshold 25). No game data or converted movies are included in this source package. Use videos from your own copy of WWE '13.

## Source release

Run `./make-source-archive.sh /path/to/wwe2k14-bink-convert-source.tar.gz`. The script writes a normalized tarball with sorted paths, fixed ownership, a fixed timestamp (`SOURCE_DATE_EPOCH`, default `0`), and gzip metadata removed. Keep the archive outside this source directory. Repeating the command with identical source files and the same `SOURCE_DATE_EPOCH` produces identical archive bytes.

## Notices and licence

This converter and the bundled modified NihAV code are distributed under the GNU Affero General Public License, version 3 only. Keep `LICENSE`, `NOTICE.md`, `LICENSES/`, and the vendored notices with binary distributions. The corresponding source is the complete `tools/bink-convert` tree or the reproducible source archive.

The AGPL applies to the converter and its bundled decoder. Running it as a child process avoids linking NihAV into the launcher, but that separation alone does not settle whether a particular combined distribution is one work or an aggregate. The GNU FAQ says this depends on the communication and how closely the programs operate as one; the launcher uses a command line and a small text progress protocol. If the launcher is distributed under different terms, review the final packaging arrangement. Any distribution of the converter binary must carry its corresponding source offer and licence notices. See [GNU AGPLv3](https://www.gnu.org/licenses/agpl-3.0.html) and the [GNU FAQ on separate programs](https://www.gnu.org/licenses/gpl-faq.html#MereAggregation).

## Release package

For each supported operating system, package the matching `wwe2k14-bink-convert` executable (`.exe` on Windows), `LICENSE`, `NOTICE.md`, and the applicable `LICENSES/` notices. Include the source archive or a direct source link with the binary release. The launcher package must place the executable next to the launcher. The Linux x86-64 build needs the system glibc version 2.34 or later, `libgcc_s`, and `libm`. Windows runtime requirements must be checked against its finished build. No wrestler videos, game files, or NihAV development tools are included.

## Building for Windows

Cross-built from Linux with the official Rust 1.75.0 compiler and the rust-std package for x86_64-pc-windows-msvc, lld-link, and the Windows SDK and MSVC CRT libraries:

    RUSTFLAGS="-C target-feature=+crt-static -C linker=lld-link -L native=<crt>/lib/x86_64 -L native=<sdk>/lib/um/x86_64 -L native=<sdk>/lib/ucrt/x86_64" \
      cargo build --release --offline --target x86_64-pc-windows-msvc

The same command works on Windows with the MSVC tools, without the linker and library flags.
