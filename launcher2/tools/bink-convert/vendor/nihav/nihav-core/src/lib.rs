//! Core functionality of NihAV intended to be used by both crates implementing format support and users.
#[cfg(feature="decoders")]
#[allow(clippy::upper_case_acronyms)]
#[allow(clippy::too_many_arguments)]
pub mod codecs;

#[cfg(feature="compr")]
pub mod compr;

#[cfg(feature="muxers")]
pub mod muxers;

#[cfg(feature="demuxers")]
pub mod demuxers;

#[allow(clippy::too_many_arguments)]
pub mod formats;
pub mod frame;
#[allow(clippy::too_many_arguments)]
pub mod io;
pub mod options;
pub mod refs;
pub mod reorder;
#[allow(clippy::upper_case_acronyms)]
pub mod scale;
pub mod soundcvt;

pub mod sbbox;
