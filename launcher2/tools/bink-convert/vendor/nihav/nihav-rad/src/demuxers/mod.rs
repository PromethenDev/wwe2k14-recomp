use nihav_core::demuxers::*;

#[allow(unused_macros)]
#[cfg(debug_assertions)]
macro_rules! validate {
    ($a:expr) => { if !$a { println!("check failed at {}:{}", file!(), line!()); return Err(DemuxerError::InvalidData); } };
}
#[cfg(not(debug_assertions))]
macro_rules! validate {
    ($a:expr) => { if !$a { return Err(DemuxerError::InvalidData); } };
}

#[cfg(feature="demuxer_smk")]
mod smacker;
#[cfg(feature="demuxer_bink")]
mod bink;

const RAD_DEMUXERS: &[&dyn DemuxerCreator] = &[
#[cfg(feature="demuxer_smk")]
    &smacker::SMKDemuxerCreator {},
#[cfg(feature="demuxer_bink")]
    &bink::BinkDemuxerCreator {},
];

/// Registers all available demuxers provided by this crate.
pub fn rad_register_all_demuxers(rd: &mut RegisteredDemuxers) {
    for demuxer in RAD_DEMUXERS.iter() {
        rd.add_demuxer(*demuxer);
    }
}
