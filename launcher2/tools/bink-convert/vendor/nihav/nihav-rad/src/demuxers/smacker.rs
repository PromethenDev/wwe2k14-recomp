use std::io::SeekFrom;
use std::cmp::Ordering;
use nihav_core::demuxers::*;

const SMK_FLAG_LOOP_FRAME: u32 = 0x01;

const SMK_FRAME_FLAG_PALETTE: u8 = 0x01;

const SMK_AUD_FLAG_PACKED:          u8 = 0x80;
const SMK_AUD_FLAG_PRESENT:         u8 = 0x40;
const SMK_AUD_FLAG_16BIT:           u8 = 0x20;
const SMK_AUD_FLAG_STEREO:          u8 = 0x10;
const SMK_AUD_FLAG_BINKAUD_RDFT:    u8 = 0x08;
const SMK_AUD_FLAG_BINKAUD_DCT:     u8 = 0x04;

const NUM_AUDIO_TRACKS: usize = 7;

const PAL_SIZE: usize = 768;

const SMK_DEFAULT_PAL: [u8; 64] = [
    0x00, 0x04, 0x08, 0x0C, 0x10, 0x14, 0x18, 0x1C,
    0x20, 0x24, 0x28, 0x2C, 0x30, 0x34, 0x38, 0x3C,
    0x41, 0x45, 0x49, 0x4D, 0x51, 0x55, 0x59, 0x5D,
    0x61, 0x65, 0x69, 0x6D, 0x71, 0x75, 0x79, 0x7D,
    0x82, 0x86, 0x8A, 0x8E, 0x92, 0x96, 0x9A, 0x9E,
    0xA2, 0xA6, 0xAA, 0xAE, 0xB2, 0xB6, 0xBA, 0xBE,
    0xC3, 0xC7, 0xCB, 0xCF, 0xD3, 0xD7, 0xDB, 0xDF,
    0xE3, 0xE7, 0xEB, 0xEF, 0xF3, 0xF7, 0xFB, 0xFF
];

#[derive(Clone,Copy)]
struct AudioTrack {
    size:       u32,
    flags:      u8,
    srate:      u32,
    id:         usize,
}

impl AudioTrack {
    fn new() -> Self {
        Self { size: 0, flags: 0, srate: 0, id: 0 }
    }
    fn is_present(&self) -> bool {
       (self.flags & SMK_AUD_FLAG_PRESENT) != 0
    }
    fn add_stream(&mut self, strmgr: &mut StreamManager, str_id: usize) -> DemuxerResult<()> {
        let channels = if (self.flags & SMK_AUD_FLAG_STEREO) != 0 { 2 } else { 1 };
        let codecname = if (self.flags & SMK_AUD_FLAG_BINKAUD_RDFT) != 0 {
                "bink-audio-rdft"
            } else if (self.flags & SMK_AUD_FLAG_BINKAUD_DCT) != 0 {
                "bink-audio-dct"
            } else if (self.flags & SMK_AUD_FLAG_PACKED) != 0 {
                "smacker-audio"
            } else {
                "pcm"
            };
        let soniton = if (self.flags & SMK_AUD_FLAG_16BIT) != 0 { SND_S16_FORMAT } else { SND_U8_FORMAT };
        let ahdr = NAAudioInfo::new(self.srate, channels, soniton, 1);
        let ainfo = NACodecInfo::new(codecname, NACodecTypeInfo::Audio(ahdr), None);
        let res = strmgr.add_stream(NAStream::new(StreamType::Audio, (str_id + 1) as u32, ainfo, 1, self.srate, 0));
        validate!(res.is_some());
        self.id = res.unwrap();

        Ok(())
    }
}

struct SmackerVideoDemuxer<'a> {
    src:            &'a mut dyn ByteIO,
    frames:         usize,
    tb_num:         u32,
    ainfo:          [AudioTrack; NUM_AUDIO_TRACKS],
    frame_sizes:    Vec<u32>,
    frame_flags:    Vec<u8>,
    video_id:       usize,
    start:          u64,
    cur_frame:      usize,
    queued_packets: Vec<NAPacket>,
    pal:            [u8; PAL_SIZE],
    force_seek:     bool,
}

/*macro_rules! mktag {
    ($a:expr, $b:expr, $c:expr, $d:expr) => ({
        (($a as u32) << 24) | (($b as u32) << 16) | (($c as u32) << 8) | ($d as u32)
    });
    ($arr:expr) => ({
        (($arr[0] as u32) << 24) | (($arr[1] as u32) << 16) | (($arr[2] as u32) << 8) | ($arr[3] as u32)
    });
}*/

fn get_pts_inc(val: i32) -> u64 {
    match val.cmp(&0) {
        Ordering::Greater => (val as u64) * 100,
        Ordering::Less    => -val as u64,
        Ordering::Equal   => 10000,
    }
}

impl<'a> DemuxCore<'a> for SmackerVideoDemuxer<'a> {
    fn open(&mut self, strmgr: &mut StreamManager, _seek_idx: &mut SeekIndex) -> DemuxerResult<()> {
        let src = &mut self.src;
        let mut magic: [u8; 4] = [0; 4];
                                              src.read_buf(&mut magic)?;
        validate!((&magic == b"SMK2") || (&magic == b"SMK4"));
        let width                           = src.read_u32le()? as usize;
        let height                          = src.read_u32le()? as usize;
        self.frames                         = src.read_u32le()? as usize;
        let pts_inc                         = src.read_u32le()? as i32;
        let flags                           = src.read_u32le()?;
        validate!((width > 0) && (height > 0) && (self.frames > 0));
        let tb_num = get_pts_inc(pts_inc);
        validate!((1..=10000000).contains(&tb_num));
        self.tb_num = tb_num as u32;

        if (flags & SMK_FLAG_LOOP_FRAME) != 0 {
            self.frames += 1;
        }

        for i in 0..NUM_AUDIO_TRACKS {
            self.ainfo[i].size              = src.read_u32le()?;
        }
        let treesize                        = src.read_u32le()? as usize;

        let mut treedata: Vec<u8> = Vec::with_capacity(treesize + 20);
        treedata.resize(treesize + 24, 0);
        let hdr = &mut treedata[0..4];
        hdr.copy_from_slice(&magic);
        let flg = &mut treedata[4..8];
        flg[0] = (flags & 0xFF) as u8;
        flg[1] = (flags >>   8) as u8;
        flg[2] = (flags >>  16) as u8;
        flg[3] = (flags >>  24) as u8;
        src.read_buf(&mut treedata[8..][..16])?;

        for i in 0..NUM_AUDIO_TRACKS {
            self.ainfo[i].srate             = src.read_u24le()?;
            self.ainfo[i].flags             = src.read_byte()?;
        }

        src.read_skip(4)?;

        self.frame_sizes.resize(self.frames, 0);
        self.frame_flags.resize(self.frames, 0);
                                              src.read_u32le_arr(&mut self.frame_sizes)?;
                                              src.read_buf(&mut self.frame_flags)?;

        src.read_buf(&mut treedata[24..])?;

        let vhdr = NAVideoInfo::new(width, height, false, PAL8_FORMAT);
        let vinfo = NACodecInfo::new("smacker-video", NACodecTypeInfo::Video(vhdr), Some(treedata));
        let res = strmgr.add_stream(NAStream::new(StreamType::Video, 0, vinfo, self.tb_num, 100000, self.frames as u64));
        validate!(res.is_some());
        self.video_id = res.unwrap();

        for i in 0..NUM_AUDIO_TRACKS {
            if self.ainfo[i].is_present() {
                self.ainfo[i].add_stream(strmgr, i)?;
            }
        }

        self.start = src.tell();
        self.cur_frame = 0;
        self.reset_state();

        Ok(())
    }
    #[allow(clippy::identity_op)]
    fn get_frame(&mut self, strmgr: &mut StreamManager) -> DemuxerResult<NAPacket> {
        if !self.queued_packets.is_empty() {
            let pkt = self.queued_packets.pop().unwrap();
            return Ok(pkt);
        }
        if self.cur_frame >= self.frames { return Err(DemuxerError::EOF); }

        let mut payload_size = (self.frame_sizes[self.cur_frame] & !3) as usize;
        let frame_flags = self.frame_flags[self.cur_frame];
        if (frame_flags & SMK_FRAME_FLAG_PALETTE) != 0 {
            let chunk_size                      = (self.src.read_byte()? as usize) * 4;
            validate!(chunk_size > 0);
            validate!(payload_size >= chunk_size);
            payload_size -= chunk_size;
            let oldpal = self.pal;
            let mut idx = 0;
            let endpos = self.src.tell() + (chunk_size as u64) - 1;
            while idx < 256 {
                let op                          = self.src.read_byte()?;
                if (op & 0x80) != 0 {
                    idx += ((op & 0x7F) as usize) + 1;
                } else if (op & 0x40) != 0 {
                    let start                   = self.src.read_byte()? as usize;
                    let len = ((op & 0x3F) as usize) + 1;
                    validate!(idx + len <= 256);
                    for i in 0..len*3 {
                        self.pal[idx * 3 + i] = oldpal[start * 3 + i];
                    }
                    idx += len;
                } else {
                    let ix0 = op as usize;
                    let ix1                     = (self.src.read_byte()? & 0x3F) as usize;
                    let ix2                     = (self.src.read_byte()? & 0x3F) as usize;
                    self.pal[idx * 3 + 0] = SMK_DEFAULT_PAL[ix0];
                    self.pal[idx * 3 + 1] = SMK_DEFAULT_PAL[ix1];
                    self.pal[idx * 3 + 2] = SMK_DEFAULT_PAL[ix2];
                    idx += 1;
                }
            }
            validate!(self.src.tell() <= endpos);
            if self.src.tell() < endpos {
                let skip_size = endpos - self.src.tell();
                self.src.read_skip(skip_size as usize)?;
            }
        }

        let ts = NATimeInfo::new(Some(self.cur_frame as u64), None, None, self.tb_num, 100000);
        for i in 0..NUM_AUDIO_TRACKS {
            if ((frame_flags >> (i + 1)) & 1) == 0 { continue; }
            let size                            = self.src.read_u32le()? as usize;
            validate!(size > 4);
            validate!(payload_size >= size);
            payload_size -= size;
            if !self.ainfo[i].is_present() {
                self.src.read_skip(size - 4)?;
                continue;
            }
            let strres = strmgr.get_stream(self.ainfo[i].id);
            validate!(strres.is_some());
            let stream = strres.unwrap();
            let pkt = self.src.read_packet(stream.clone(), ts, true, size - 4)?;
            self.queued_packets.push(pkt);
        }
        self.queued_packets.reverse();

        let mut buf: Vec<u8> = Vec::with_capacity(PAL_SIZE + payload_size);
        buf.resize(PAL_SIZE, 0);
        buf.copy_from_slice(&self.pal[0..]);
        if payload_size > 0 {
            buf.resize(PAL_SIZE + payload_size, 0);
            self.src.read_buf(&mut buf[PAL_SIZE..])?;
        }

        let strres = strmgr.get_stream(self.video_id);
        validate!(strres.is_some());
        let stream = strres.unwrap();
        let keyframe = (self.frame_sizes[self.cur_frame] & 1) != 0;
        let pkt = NAPacket::new(stream, ts, keyframe, buf);

        self.cur_frame += 1;

        Ok(pkt)
    }
    fn seek(&mut self, time: NATimePoint, _seek_idx: &SeekIndex) -> DemuxerResult<()> {
        let ts = match time {
                NATimePoint::Milliseconds(ms) => ms * 100 / u64::from(self.tb_num),
                NATimePoint::PTS(pts) => pts,
                _ => return Err(DemuxerError::SeekError),
            };
        if ts >= (self.frames as u64) || (!self.force_seek && ts > 0) {
            return Err(DemuxerError::SeekError);
        }
        let ts = ts as usize;

        let start_pos = self.frame_sizes[..ts].iter()
                .fold(self.start, |acc, &sz| acc + u64::from(sz & !3));
        self.src.seek(SeekFrom::Start(start_pos))?;
        self.cur_frame = ts;
        self.reset_state();
        Ok(())
    }
    fn get_duration(&self) -> u64 { 0 }
}

const DEMUXER_OPTS: &[NAOptionDefinition] = &[
    NAOptionDefinition {
        name: FORCE_SEEK_OPTION, description: FORCE_SEEK_OPTION_DESC,
        opt_type: NAOptionDefinitionType::Bool },
];

impl<'a> NAOptionHandler for SmackerVideoDemuxer<'a> {
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

impl<'a> SmackerVideoDemuxer<'a> {
    fn new(io: &'a mut dyn ByteIO) -> Self {
        SmackerVideoDemuxer {
            src:            io,
            frames:         0,
            tb_num:         0,
            ainfo:          [AudioTrack::new(); NUM_AUDIO_TRACKS],
            frame_sizes:    Vec::new(),
            frame_flags:    Vec::new(),
            video_id:       0,
            start:          0,
            cur_frame:      0,
            queued_packets: Vec::new(),
            pal:            [0; PAL_SIZE],
            force_seek:     false,
        }
    }
    fn reset_state(&mut self) {
        self.queued_packets.clear();
    }
}

pub struct SMKDemuxerCreator { }

impl DemuxerCreator for SMKDemuxerCreator {
    fn new_demuxer<'a>(&self, br: &'a mut dyn ByteIO) -> Box<dyn DemuxCore<'a> + 'a> {
        Box::new(SmackerVideoDemuxer::new(br))
    }
    fn get_name(&self) -> &'static str { "smacker" }
}

#[cfg(test)]
mod test {
    use super::*;
    use nihav_codec_support::test::dmx_test::*;

    #[test]
    fn test_smk_demux() {
        // sample: https://samples.mplayerhq.hu/game-formats/smacker/20130507_audio-distortion.smk
        let file = "assets/RAD/20130507_audio-distortion.smk";
//        let file = "assets/RAD/ajfstr1.smk";
//        let file = "assets/RAD/credits.smk";
//        let file = "assets/RAD/wetlogo.smk";
        test_file_demux(file, &SMKDemuxerCreator{}, 0, DemuxerTestMode::Demux);
    }
}
