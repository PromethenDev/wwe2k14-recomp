//! Code and data for easier development of NihAV decoders.
pub mod codecs;

#[cfg(feature="dsp")]
pub mod dsp;

pub mod data;

#[allow(clippy::identity_op)]
pub mod imgwrite;

#[allow(clippy::too_many_arguments)]
#[allow(clippy::type_complexity)]
pub mod test;

#[cfg(feature="vq")]
pub mod vq;

extern crate nihav_core;
