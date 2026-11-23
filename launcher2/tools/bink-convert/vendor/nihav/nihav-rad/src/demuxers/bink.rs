use nihav_core::demuxers::*;
use nihav_core::io::byteio::ByteIO;

struct AudioTrack {
    id:             usize,
}

const BINK_AUD_FLAG_DCT:    u8 = 0x10;
const BINK_AUD_FLAG_STEREO: u8 = 0x20;

impl AudioTrack {
    fn new(strmgr: &mut StreamManager, srate: u32, flags: u8, str_id: usize, magic: [u8; 4]) -> DemuxerResult<Self> {
        let channels = if (flags & BINK_AUD_FLAG_STEREO) != 0 { 2 } else { 1 };
        let codecname = if (flags & BINK_AUD_FLAG_DCT) != 0 {
                "bink-audio-dct"
            } else {
                "bink-audio-rdft"
            };
        let ahdr = NAAudioInfo::new(srate, channels, SND_F32P_FORMAT, 1);
        let mut edata: Vec<u8> = Vec::with_capacity(4);
        edata.extend_from_slice(&magic);
        let ainfo = NACodecInfo::new(codecname, NACodecTypeInfo::Audio(ahdr), Some(edata));
        let res = strmgr.add_stream(NAStream::new(StreamType::Audio, (str_id + 1) as u32, ainfo, 1, srate, 0));
        validate!(res.is_some());
        let id = res.unwrap();
        Ok(Self{ id })
    }
}

struct BinkDemuxer<'a> {
    src:            &'a mut dyn ByteIO,
    cur_frame:      usize,
    frames:         usize,
    video_id:       usize,
    tb_num:         u32,
    tb_den:         u32,
    ainfo:          Vec<AudioTrack>,
    queued_packets: Vec<NAPacket>,
    frame_pos:      Vec<u32>,
    force_seek:     bool,
}

macro_rules! mktag {
    ($a:expr, $b:expr, $c:expr, $d:expr) => ({
        (($a as u32) << 24) | (($b as u32) << 16) | (($c as u32) << 8) | ($d as u32)
    });
    ($arr:expr) => ({
        (($arr[0] as u32) << 24) | (($arr[1] as u32) << 16) | (($arr[2] as u32) << 8) | ($arr[3] as u32)
    });
}

impl<'a> DemuxCore<'a> for BinkDemuxer<'a> {
    fn open(&mut self, strmgr: &mut StreamManager, seek_idx: &mut SeekIndex) -> DemuxerResult<()> {
        let src = &mut self.src;
        let mut magic: [u8; 4] = [0; 4];
                                              src.read_buf(&mut magic)?;
        let magic_tag = mktag!(magic);
        validate!((magic_tag >= mktag!(b"BIKb") && magic_tag <= mktag!(b"BIKk")) ||
                  (magic_tag >= mktag!(b"KB2a") && magic_tag <= mktag!(b"KB2k")));
        let fsize                           = (src.read_u32le()? as usize) + 8;
        self.frames                         = src.read_u32le()? as usize;
        let max_size                        = src.read_u32le()? as usize;
        let _frames2                        = src.read_u32le()? as usize;
        let width                           = src.read_u32le()? as usize;
        let height                          = src.read_u32le()? as usize;
        let tb_den                          = src.read_u32le()?;
        let tb_num                          = src.read_u32le()?;
        validate!((width > 0) && (height > 0) && (width <= 7680) && (height <= 4800));
        validate!((self.frames > 0) && (tb_num > 0) && (tb_den > 0) && (max_size < fsize));
        self.tb_num = tb_num;
        self.tb_den = tb_den;
        let mut flags: [u8; 4] = [0; 4];
                                              src.read_buf(&mut flags)?;
        let mut edata: Vec<u8> = vec![0; 8];
        let p0 = &mut edata[0..4];
        p0.copy_from_slice(&magic);
        let p1 = &mut edata[4..][..4];
        p1.copy_from_slice(&flags);
        let vhdr = NAVideoInfo::new(width, height, false, YUV420_FORMAT);
        let codec = if magic[0] == b'K' && magic[1] == b'B' && magic[2] == b'2' { "bink2-video" } else { "bink-video" };
        let vinfo = NACodecInfo::new(codec, NACodecTypeInfo::Video(vhdr), Some(edata));
        let res = strmgr.add_stream(NAStream::new(StreamType::Video, 0, vinfo, self.tb_num, self.tb_den, self.frames as u64));
        validate!(res.is_some());
        self.video_id = res.unwrap();

        let num_audio                       = src.read_u32le()? as usize;
        if magic_tag >= mktag!(b"KB2i") {
                                              src.read_skip(4)?;
        }
        validate!(num_audio < 256);
                                              src.read_skip(num_audio * 4)?; // audio max output frame size?
        self.ainfo = Vec::with_capacity(num_audio);
        for i in 0..num_audio {
            let srate                       = src.read_u24le()?;
            let flags                       = src.read_byte()?;
            validate!(srate > 0);
            self.ainfo.push(AudioTrack::new(strmgr, srate, flags, i, magic)?);
        }
        for _ in 0..num_audio {
            let _trk_id                     = src.read_u32le()?;
        }

        seek_idx.mode = SeekIndexMode::Present;
        seek_idx.add_stream(0);
        self.frame_pos = Vec::with_capacity(self.frames + 1);
        for fno in 0..=self.frames {
            let pos                         = src.read_u32le()?;
            self.frame_pos.push(pos);
            if (pos & 1) != 0 {
                let time = (fno as u64) * 1000 * (tb_num as u64) / (tb_den as u64);
                seek_idx.seek_info[0].add_entry(SeekEntry { time, pts: fno as u64, pos: (pos & !1) as u64 });
            }
        }
        validate!((src.tell() as u32) == (self.frame_pos[0] & !1));
        seek_idx.seek_info[0].filled = true;

        self.cur_frame = 0;

        Ok(())
    }
    fn get_frame(&mut self, strmgr: &mut StreamManager) -> DemuxerResult<NAPacket> {
        if !self.queued_packets.is_empty() {
            let pkt = self.queued_packets.pop().unwrap();
            return Ok(pkt);
        }
        if self.cur_frame >= self.frames { return Err(DemuxerError::EOF); }
        let mut payload_size = ((self.frame_pos[self.cur_frame + 1] & !1) - (self.frame_pos[self.cur_frame] & !1)) as usize;
        validate!(payload_size > self.ainfo.len() * 4);
        for atrk in self.ainfo.iter() {
            let size                            = self.src.read_u32le()? as usize;
            validate!(payload_size > size + 4);
            payload_size -= size + 4;

            if size > 0 {
                let strres = strmgr.get_stream(atrk.id);
                validate!(strres.is_some());
                let stream = strres.unwrap();
                let ts = NATimeInfo::new(Some(self.cur_frame as u64), None, None, self.tb_num, self.tb_den);
                let pkt = self.src.read_packet(stream.clone(), ts, true, size)?;
                self.queued_packets.push(pkt);
            }
        }
        self.queued_packets.reverse();

        let strres = strmgr.get_stream(self.video_id);
        validate!(strres.is_some());
        let stream = strres.unwrap();
        let keyframe = (self.frame_pos[self.cur_frame] & 1) != 0;
        let ts = NATimeInfo::new(Some(self.cur_frame as u64), None, None, self.tb_num, self.tb_den);
        let pkt = self.src.read_packet(stream, ts, keyframe, payload_size)?;

        self.cur_frame += 1;

        Ok(pkt)
    }
    fn seek(&mut self, time: NATimePoint, seek_idx: &SeekIndex) -> DemuxerResult<()> {
        let seek_ts = match time {
                NATimePoint::Milliseconds(ms) => NATimeInfo::rescale_ts(ms, 1, 1000, self.tb_num, self.tb_den),
                NATimePoint::PTS(ts) => ts,
                _ => return Err(DemuxerError::SeekError),
            };
        if seek_ts >= (self.frames as u64) {
            return Err(DemuxerError::SeekError);
        }
        if let Some(seek_info) = seek_idx.find_pos(time) {
            if !self.force_seek || seek_info.pts.abs_diff(seek_ts) <= u64::from(self.tb_den * 2 / self.tb_num.max(1)) {
                self.src.seek(SeekFrom::Start(seek_info.pos))?;
                self.queued_packets.clear();
                self.cur_frame = seek_info.pts as usize;
                return Ok(());
            }
        }
        if !self.force_seek {
            return Err(DemuxerError::SeekError);
        }
        self.cur_frame = seek_ts as usize;
        self.src.seek(SeekFrom::Start(u64::from(self.frame_pos[self.cur_frame] & !1)))?;
        self.queued_packets.clear();
        Ok(())
    }
    fn get_duration(&self) -> u64 { 0 }
}

const DEMUXER_OPTS: &[NAOptionDefinition] = &[
    NAOptionDefinition {
        name: FORCE_SEEK_OPTION, description: FORCE_SEEK_OPTION_DESC,
        opt_type: NAOptionDefinitionType::Bool },
];

impl<'a> NAOptionHandler for BinkDemuxer<'a> {
    fn get_supported_options(&self) -> &[NAOptionDefinition] { DEMUXER_OPTS }
    fn set_options(&mut self, options: &[NAOption]) {
        for option in options.iter() {
            if let NAOption { name: FORCE_SEEK_OPTION, value: NAValue::Bool(ref bval) } = option {
                self.force_seek = *bval;
            }
        }
    }
    fn query_option_value(&self, name: &str) -> Option<NAValue> {
        if name == FORCE_SEEK_OPTION {
            Some(NAValue::Bool(self.force_seek))
        } else {
            None
        }
    }
}

impl<'a> BinkDemuxer<'a> {
    fn new(io: &'a mut dyn ByteIO) -> Self {
        Self {
            src:            io,
            cur_frame:      0,
            frames:         0,
            video_id:       0,
            tb_num:         0,
            tb_den:         0,
            ainfo:          Vec::new(),
            queued_packets: Vec::new(),
            frame_pos:      Vec::new(),
            force_seek:     false,
        }
    }
}


pub struct BinkDemuxerCreator { }

impl DemuxerCreator for BinkDemuxerCreator {
    fn new_demuxer<'a>(&self, br: &'a mut dyn ByteIO) -> Box<dyn DemuxCore<'a> + 'a> {
        Box::new(BinkDemuxer::new(br))
    }
    fn get_name(&self) -> &'static str { "bink" }
}

#[cfg(test)]
mod test {
    use super::*;
    use nihav_codec_support::test::dmx_test::*;

    #[test]
    fn test_bink_demux() {
        // sample: https://samples.mplayerhq.hu/game-formats/bink/ActivisionLogo.bik
        let file = "assets/RAD/ActivisionLogo.bik";
//        let file = "assets/RAD/original.bik";
//        let file = "assets/RAD/Snd0a110c51.dee";
//        let file = "assets/RAD/NEW.BIK";
//        let file = "assets/RAD/ge_video_86l.bk2";
        test_file_demux(file, &BinkDemuxerCreator{}, 0, DemuxerTestMode::Demux);
    }
}
