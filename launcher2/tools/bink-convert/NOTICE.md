# Copyright and dependency notices

## Converter

Copyright (C) 2026 WWE 2K14 Native contributors. The BIKi writer was copied from the project owner's previously private `bk2enc` source and is published here under the GNU Affero General Public License, version 3 only. Its scan and quantiser tables come from NihAV's `nihav-rad/src/codecs/binkviddata.rs` and remain covered by NihAV's copyright and licence below.

## NihAV

This source package includes the `nihav_core`, `nihav_codec_support`, and `nihav_rad` crates from the NihAV source snapshot named `nihav-HEAD-63ac44b`. Their Cargo manifests retain the author attribution: Kostya Shishkov <kostya.shishkov@gmail.com>. NihAV is licensed under the GNU Affero General Public License, version 3; the full upstream `COPYING` is included at `vendor/nihav/COPYING`, and the upstream README is included at `vendor/nihav/README.md`.

`nihav-rad/src/codecs/bink2.rs` includes the WWE '13 KB2d decoder patch. The patch file is preserved at `vendor/nihav/nihav-kb2d.patch`; it describes the added KB2d macroblock and coefficient decoding path. This patched code is part of the AGPL-covered converter. It also includes the WWE 2K15 KB2i fixes (inter DC rounding bias and 2D chroma interpolation rounding for KB2i and newer), preserved at `vendor/nihav/nihav-kb2i.patch`.

The copied `nihav-rad/Cargo.toml` omits only its development-only `nihav_commonfmt` dependency; the converter does not build or use that test dependency. The three compiled NihAV crates depend only on each other and Rust's standard library. There are no crates.io packages or other third-party libraries in `Cargo.lock`.

## Rust runtime

The executable is built with Rust and statically incorporates the Rust standard library supplied by the chosen Rust toolchain. The Rust standard library and its components carry multiple notices and licences; the complete copyright inventory for the Linux build's Rust 1.75 standard library is included at `LICENSES/RUST-STDLIB-COPYRIGHT-1.75`. That inventory records MIT/Apache-2.0 notices and the component-specific terms. A Windows release built with another toolchain must include that toolchain's applicable runtime notices. No Rust runtime DLL is bundled by this project.

## Game content

No WWE game assets, source videos, converted videos, executables, or user data are included. Users supply their own input files.
