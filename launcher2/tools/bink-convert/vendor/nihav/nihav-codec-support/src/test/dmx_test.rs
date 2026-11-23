//! Demuxer testing functionality.
use nihav_core::demuxers::*;

#[derive(Clone,Copy,Debug,PartialEq)]
pub struct DPacket {
    pub stream: u32,
    pub pts:    Option<u64>,
    pub dts:    Option<u64>,
    pub size:   usize,
    pub kf:     bool,
}

impl DPacket {
    pub fn new(stream: u32, pts: Option<u64>, dts: Option<u64>, size: usize, kf: bool) -> Self {
        Self { stream, pts, dts, size, kf }
    }
}

pub enum DemuxerTestMode {
    Demux,
    Generate,
    Test(Vec<DPacket>),
}

pub fn test_file_demux(name: &str, dmx_cr: &dyn DemuxerCreator, npkts: usize, mode: DemuxerTestMode) {
    let mut file = std::fs::File::open(name).unwrap();
    let mut br = FileReader::new_read(&mut file);
    let mut dmx = dmx_cr.new_demuxer(&mut br);
    let mut sm = StreamManager::new();
    let mut si = SeekIndex::new();
    dmx.open(&mut sm, &mut si).unwrap();
    test_demuxer(dmx.as_mut(), &mut sm, npkts, mode);
}

pub fn test_demuxer(dmx: &mut dyn DemuxCore, sm: &mut StreamManager, mut npkts: usize, mode: DemuxerTestMode) {
    if npkts == 0 {
        npkts = usize::MAX;
    }
    match mode {
        DemuxerTestMode::Demux => {
            for _ in 0..npkts {
                match dmx.get_frame(sm) {
                    Ok(pkt) => println!("Got {pkt}"),
                    Err(DemuxerError::EOF) => break,
                    Err(err) => panic!("demuxer error {:?}", err),
                }
            }
        },
        DemuxerTestMode::Generate => {
            for _ in 0..npkts {
                match dmx.get_frame(sm) {
                    Ok(pkt) => println!("            DPacket::new({}, {:?}, {:?}, {}, {}),", pkt.get_stream().get_id(), pkt.ts.pts, pkt.ts.dts, pkt.get_buffer().len(), pkt.keyframe),
                    Err(DemuxerError::EOF) => break,
                    Err(err) => panic!("demuxer error {:?}", err),
                }
            }
            panic!("Generated data");
        },
        DemuxerTestMode::Test(mdata) => {
            for refpkt in mdata.iter().take(npkts) {
                let pkt = dmx.get_frame(sm).unwrap();
                let ndt = DPacket::new(pkt.get_stream().get_id(), pkt.ts.pts, pkt.ts.dts, pkt.get_buffer().len(), pkt.keyframe);
                assert_eq!(refpkt, &ndt);
            }
        },
    }
}
