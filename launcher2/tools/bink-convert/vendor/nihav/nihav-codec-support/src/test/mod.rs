//! Decoder testing functionality.
//!
//! This module provides functions that may be used in internal test to check that decoders produce output and that they produce expected output.
#[allow(clippy::identity_op)]
pub mod dec_video;
pub mod enc_video;
pub mod dmx_test;
pub mod wavwriter;

#[allow(clippy::identity_op)]
#[allow(clippy::unreadable_literal)]
mod md5; // for internal checksums only

/// Decoder testing modes.
///
/// NihAV has its own MD5 hasher that calculates hash for input frame in a deterministic way (i.e. no endianness issues) and it outputs hash as `[u32; 4]`. During testing the resulting hash will be printed out so when the test fails you can find what was the calculated hash (and use it as a reference if you are implementing a new test).
pub enum ExpectedTestResult {
    /// Decoding runs without errors.
    Decodes,
    /// Full decoded output is expected to be equal to this MD5 hash.
    MD5([u32; 4]),
    /// Each decoded frame hash should be equal to the corresponding MD5 hash.
    MD5Frames(Vec<[u32; 4]>),
    /// Test function should report decoded frame hashes to be used as the reference later.
    GenerateMD5Frames,
}
