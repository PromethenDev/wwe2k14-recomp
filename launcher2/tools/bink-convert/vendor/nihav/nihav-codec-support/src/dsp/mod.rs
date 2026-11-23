//! DSP routines.
#[cfg(feature="dct")]
#[allow(clippy::erasing_op)]
pub mod dct;
#[cfg(feature="fft")]
#[allow(clippy::erasing_op)]
pub mod fft;
#[cfg(feature="lpc")]
pub mod lpc;
#[cfg(feature="mdct")]
pub mod mdct;
#[cfg(feature="qmf")]
pub mod qmf;
#[cfg(feature="dsp_window")]
pub mod window;
