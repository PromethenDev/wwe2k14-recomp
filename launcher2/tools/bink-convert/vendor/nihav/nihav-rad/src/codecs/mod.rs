use nihav_core::codecs::*;

#[cfg(debug_assertions)]
macro_rules! validate {
    ($a:expr) => { if !$a { println!("check failed at {}:{}", file!(), line!()); return Err(DecoderError::InvalidData); } };
}
#[cfg(not(debug_assertions))]
macro_rules! validate {
    ($a:expr) => { if !$a { return Err(DecoderError::InvalidData); } };
}

#[cfg(any(feature="decoder_smkaud", feature="decoder_smkvid"))]
#[allow(dead_code)]
mod smacker;
#[cfg(feature="decoder_binkaud")]
mod binkaud;
#[cfg(any(feature="decoder_binkaud", feature="encoder_binkaud"))]
mod binkauddata;
#[cfg(feature="decoder_binkvid")]
mod binkvid;
#[cfg(any(feature="decoder_binkvid", feature="encoder_binkvid"))]
mod binkviddata;
#[cfg(feature="decoder_bink2")]
#[allow(clippy::too_many_arguments)]
mod bink2;

const RAD_CODECS: &[DecoderInfo] = &[
#[cfg(feature="decoder_smkaud")]
    DecoderInfo { name: "smacker-audio", get_decoder: smacker::get_decoder_audio },
#[cfg(feature="decoder_smkvid")]
    DecoderInfo { name: "smacker-video", get_decoder: smacker::get_decoder_video },
#[cfg(feature="decoder_binkaud")]
    DecoderInfo { name: "bink-audio-dct", get_decoder: binkaud::get_decoder_dct },
#[cfg(feature="decoder_binkaud")]
    DecoderInfo { name: "bink-audio-rdft", get_decoder: binkaud::get_decoder_rdft },
#[cfg(feature="decoder_binkvid")]
    DecoderInfo { name: "bink-video", get_decoder: binkvid::get_decoder },
#[cfg(feature="decoder_bink2")]
    DecoderInfo { name: "bink2-video", get_decoder: bink2::get_decoder },
];

/// Registers all available codecs provided by this crate.
pub fn rad_register_all_decoders(rd: &mut RegisteredDecoders) {
    for decoder in RAD_CODECS.iter() {
        rd.add_decoder(*decoder);
    }
}

#[cfg(feature="encoder_binkvid")]
mod binkvidenc;
#[cfg(feature="encoder_binkaud")]
mod binkaudenc;

#[cfg(feature="encoders")]
const ENCODERS: &[EncoderInfo] = &[
#[cfg(feature="encoder_binkvid")]
    EncoderInfo { name: "bink-video", get_encoder: binkvidenc::get_encoder },
#[cfg(feature="encoder_binkaud")]
    EncoderInfo { name: "bink-audio-rdft", get_encoder: binkaudenc::get_encoder_rdft },
//#[cfg(feature="encoder_binkvid")]
//    EncoderInfo { name: "bink-audio-dct", get_encoder: binkaudenc::get_encoder_dct },
];

/// Registers all available encoders provided by this crate.
#[cfg(feature="encoders")]
pub fn rad_register_all_encoders(re: &mut RegisteredEncoders) {
    for encoder in ENCODERS.iter() {
        re.add_encoder(*encoder);
    }
}
