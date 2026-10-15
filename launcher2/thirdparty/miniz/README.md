# miniz

`miniz.h` is derived from the SDK SDL3 source tree's single-header miniz v1.15 implementation. SDL's
copy disables standard I/O, inflate, ZIP archive, and ZIP writing APIs for its PNG-only use; this
copy removes those SDL-specific feature-disable macros so the standalone declarations and
implementation include ZIP reader/writer support. The upstream header carries a public-domain
dedication, which is retained. The UI does not use miniz directly.
