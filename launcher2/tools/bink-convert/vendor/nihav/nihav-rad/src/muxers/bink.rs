use nihav_core::muxers::*;
use std::collections::VecDeque;

const ASSEMBLE_DEPTH: usize = 32;

struct BinkMuxer<'a> {
    bw:             &'a mut dyn ByteIO,
    naudio:         usize,
    maxsize:        u32,
    nframes:        usize,
    index:          Vec<u32>,
    index_start:    u64,
    data_start:     u64,

    video_parts:    VecDeque<(NABufferRef<Vec<u8>>, bool)>,
    audio_parts:    Vec<VecDeque<NABufferRef<Vec<u8>>>>,
    alen:           Vec<u32>,
}

impl<'a> BinkMuxer<'a> {
    fn new(bw: &'a mut dyn ByteIO) -> Self {
        Self {
            bw,
            naudio:         0,
            maxsize:        0,
            nframes:        0,
            index:          Vec::new(),
            index_start:    0,
            data_start:     0,

            video_parts:    VecDeque::with_capacity(ASSEMBLE_DEPTH),
            audio_parts:    Vec::new(),
            alen:           Vec::new(),
        }
    }
    fn write_frame(&mut self, flush: bool) -> MuxerResult<bool> {
        if self.video_parts.is_empty() {
            return Ok(false);
        }
        let has_audio = flush || self.audio_parts.iter().fold(true, |acc, apart| acc & !apart.is_empty());

        if !has_audio {
            return Ok(false);
        }

        let (vbuf, kf) = self.video_parts.pop_front().unwrap();

        let start = self.bw.tell();

        for (apart, maxalen) in self.audio_parts.iter_mut().zip(self.alen.iter_mut()) {
            if let Some(buf) = apart.pop_front() {
                validate!(buf.len() > 4);
                let alen = read_u32le(&buf)?;
                *maxalen = (*maxalen).max(alen);
                                self.bw.write_u32le(buf.len() as u32)?;
                                self.bw.write_buf(&buf)?;
            } else {
                                self.bw.write_u32le(0)?;
            }
        }
                                self.bw.write_buf(&vbuf)?;
        if (self.bw.tell() & 1) != 0 {
                                self.bw.write_byte(0)?; // padding
        }
        let frame_size = (self.bw.tell() - start) as u32;
        self.maxsize = self.maxsize.max(frame_size);
        let mut ix = start as u32;
        if kf {
            ix |= 1;
        }
        self.index.push(ix);

        Ok(true)
    }
}

impl<'a> MuxCore<'a> for BinkMuxer<'a> {
    fn create(&mut self, strmgr: &StreamManager) -> MuxerResult<()> {
        if strmgr.get_num_streams() == 0 {
            return Err(MuxerError::InvalidArgument);
        }
        if strmgr.get_num_streams() > 257 {
            return Err(MuxerError::UnsupportedFormat);
        }
        let vstream = strmgr.get_stream(0).unwrap();
        if vstream.get_media_type() != StreamType::Video {
            println!("bink: first stream must be video");
            return Err(MuxerError::UnsupportedFormat);
        }
        let info = vstream.get_info();
        if !matches!(info.get_name(), "bink-video" | "bink2-video") {
            return Err(MuxerError::UnsupportedFormat);
        }
        for strm in strmgr.iter().skip(1) {
            if strm.get_media_type() == StreamType::Audio {
                if !matches!(strm.get_info().get_name(), "bink-audio-dct" | "bink-audio-rdft") {
                    return Err(MuxerError::UnsupportedFormat);
                }
                self.naudio += 1;
            } else {
                return Err(MuxerError::UnsupportedFormat);
            }
        }

        //todo determine nframes properly
        self.nframes = vstream.get_duration() as usize;
        if self.nframes == 0 {
            println!("Cannot create Bink file with an undefined number of frames\n");
            return Err(MuxerError::NotImplemented);
        }

        // video header
        if let Some(edata) = info.get_extradata() {
            let vinfo = info.get_properties().get_video_info().unwrap();
            validate!(edata.len() >= 8);
                                self.bw.write_buf(&edata[..4])?;
                                self.bw.write_u32le(0)?; // file size
                                self.bw.write_u32le(0)?; // number of frames
                                self.bw.write_u32le(0)?; // largest frame size
                                self.bw.write_u32le(0)?; // number of frames again
                                self.bw.write_u32le(vinfo.width as u32)?;
                                self.bw.write_u32le(vinfo.height as u32)?;
                                self.bw.write_u32le(vstream.tb_den)?;
                                self.bw.write_u32le(vstream.tb_num)?;
                                self.bw.write_buf(&edata[4..8])?;
        } else {
            return Err(MuxerError::UnsupportedFormat);
        }

        // audio header
                                self.bw.write_u32le(self.naudio as u32)?;
//xxx: some KB2 field
        for _ in 0..self.naudio {
                                self.bw.write_u32le(0)?; // max audio frame duration
        }
        for astrm in strmgr.iter().skip(1) {
            let info = astrm.get_info();
            let ainfo = info.get_properties().get_audio_info().unwrap();
                                self.bw.write_u24le(ainfo.sample_rate)?;
            let mut flags = 0;
            if info.get_name() == "bink-audio-dct" {
                flags |= 0x10;
                flags |= 0x40;
            }
            if info.get_name() == "bink-audio-rdft" {
                flags |= 0x80;
            }
            if ainfo.channels == 2 {
                flags |= 0x20;
            }
                                self.bw.write_byte(flags)?;
        }
        for i in 0..self.naudio {
                                self.bw.write_u32le(i as u32)?;
        }

        for _ in 0..self.naudio {
            self.audio_parts.push(VecDeque::with_capacity(ASSEMBLE_DEPTH));
            self.alen.push(0);
        }

        self.index_start = self.bw.tell();
        for _ in 0..=self.nframes {
                                self.bw.write_u32le(0)?;
        }

        self.data_start = self.bw.tell();

        Ok(())
    }
    fn mux_frame(&mut self, _strmgr: &StreamManager, pkt: NAPacket) -> MuxerResult<()> {
        if self.data_start == 0 {
            return Err(MuxerError::NotCreated);
        }
        let stream = pkt.get_stream();
        let str_num = stream.get_num();
        let src = pkt.get_buffer();
        if str_num == 0 { // video
            self.video_parts.push_back((src, pkt.is_keyframe()));
            if self.video_parts.len() >= ASSEMBLE_DEPTH {
                while self.write_frame(false)? {
                }
            }
            if self.video_parts.len() >= ASSEMBLE_DEPTH {
                self.write_frame(true)?;
            }
        } else {
            self.audio_parts[str_num - 1].push_back(src);
        }

        Ok(())
    }
    fn flush(&mut self) -> MuxerResult<()> {
        Ok(())
    }
    fn end(&mut self) -> MuxerResult<()> {
        while self.write_frame(true)? {
        }

        let full_size = self.bw.tell();

        //xxx: if index.len() <= nframes move data back and patch index and full_size

        self.index.push(self.bw.tell() as u32);

                                    self.bw.seek(SeekFrom::Start(4))?;
                                    self.bw.write_u32le((full_size - 8) as u32)?;
                                    self.bw.write_u32le(self.index.len() as u32 - 1)?;
                                    self.bw.write_u32le(self.maxsize)?;
                                    self.bw.write_u32le(self.index.len() as u32 - 1)?;

                                    self.bw.seek(SeekFrom::Start(44))?; //xxx: or 48 for Bink2
        for &alen in self.alen.iter() {
                                    self.bw.write_u32le(alen)?;
        }

                                    self.bw.seek(SeekFrom::Start(self.index_start))?;
        for &idx in self.index.iter() {
                                    self.bw.write_u32le(idx)?;
        }
        Ok(())
    }
}

impl<'a> NAOptionHandler for BinkMuxer<'a> {
    fn get_supported_options(&self) -> &[NAOptionDefinition] { &[] }
    fn set_options(&mut self, _options: &[NAOption]) { }
    fn query_option_value(&self, _name: &str) -> Option<NAValue> { None }
}

pub struct BinkMuxerCreator {}

impl MuxerCreator for BinkMuxerCreator {
    fn new_muxer<'a>(&self, bw: &'a mut dyn ByteIO) -> Box<dyn MuxCore<'a> + 'a> {
        Box::new(BinkMuxer::new(bw))
    }
    fn get_name(&self) -> &'static str { "bink" }
    fn get_capabilities(&self) -> MuxerCapabilities { MuxerCapabilities::Universal }
    fn get_quirks(&self) -> MuxerQuirks { MuxerQuirks(MUX_QUIRK_FIXED_RATE | MUX_QUIRK_FIXED_DURATION) }
}

#[cfg(test)]
mod test {
    use nihav_core::codecs::*;
    use nihav_core::demuxers::*;
    use nihav_core::muxers::*;
    use nihav_codec_support::test::enc_video::*;
    use crate::*;

    #[test]
    fn test_bink_muxer() {
        let mut dmx_reg = RegisteredDemuxers::new();
        rad_register_all_demuxers(&mut dmx_reg);
        //test sample: https://samples.mplayerhq.hu/game-formats/bink/ActivisionLogo
        let dec_config = DecoderTestParams {
                demuxer:        "bink",
                in_name:        "assets/RAD/ActivisionLogo.bik",
                limit:          None,
                stream_type:    StreamType::None,
                dmx_reg, dec_reg: RegisteredDecoders::new(),
            };
        let mut mux_reg = RegisteredMuxers::new();
        rad_register_all_muxers(&mut mux_reg);
        /*let enc_config = EncoderTestParams {
                muxer:      "bink",
                enc_name:   "",
                out_name:   "muxed.bink",
                mux_reg, enc_reg: RegisteredEncoders::new(),
            };
        test_remuxing(&dec_config, &enc_config);*/
        test_remuxing_md5(&dec_config, "bink", &mux_reg,
                          [0x98e1892c, 0xcac2f087, 0x7350256e, 0xc71c927a]);
    }
}
