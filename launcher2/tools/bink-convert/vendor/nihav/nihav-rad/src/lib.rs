//! Crate for providing support for Bink and Smacker formats.
extern crate nihav_core;
extern crate nihav_codec_support;

#[cfg(feature="decoders")]
#[allow(clippy::excessive_precision)]
#[allow(clippy::upper_case_acronyms)]
mod codecs;
#[cfg(feature="decoders")]
pub use crate::codecs::rad_register_all_decoders;
#[cfg(feature="encoders")]
pub use crate::codecs::rad_register_all_encoders;

#[cfg(feature="demuxers")]
mod demuxers;
#[cfg(feature="demuxers")]
pub use crate::demuxers::rad_register_all_demuxers;

#[cfg(feature="muxers")]
mod muxers;
#[cfg(feature="muxers")]
pub use crate::muxers::rad_register_all_muxers;
