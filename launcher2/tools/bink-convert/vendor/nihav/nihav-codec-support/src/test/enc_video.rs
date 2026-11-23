//! Routines for testing encoders and muxers.
use std::fs::File;
use nihav_core::frame::*;
use nihav_core::codecs::*;
use nihav_core::demuxers::*;
use nihav_core::muxers::*;
use nihav_core::scale::*;
use nihav_core::soundcvt::*;
use super::md5::MD5;

/// Parameters for the source used in the test.
pub struct DecoderTestParams {
    /// Demuxer name e.g. `"mov"`.
    pub demuxer:        &'static str,
    /// Input file name.
    pub in_name:        &'static str,
    /// Timestamp for last decoded frame.
    pub limit:          Option<u64>,
    /// Desired input stream type (that will be decoded and fed to the encoder).
    pub stream_type:    StreamType,
    /// Registered demuxers.
    pub dmx_reg:        RegisteredDemuxers,
    /// Registered decoders.
    pub dec_reg:        RegisteredDecoders,
}

/// Parameters for the encoding test output.
pub struct EncoderTestParams {
    /// Muxer name e.g. `"avi"`.
    pub muxer:          &'static str,
    /// Encoder name.
    pub enc_name:       &'static str,
    /// Output file name.
    pub out_name:       &'static str,
    /// Registered muxers.
    pub mux_reg:        RegisteredMuxers,
    /// Registered encoders.
    pub enc_reg:        RegisteredEncoders,
}

/// Tests muxer by making it mux raw streams from the input file.
///
/// Streams not fitting the output profile (e.g. a video stream or a second audio stream for WAV muxer) will be ignored.
pub fn test_remuxing(dec_config: &DecoderTestParams, enc_config: &EncoderTestParams) {
    let dmx_f = dec_config.dmx_reg.find_demuxer(dec_config.demuxer).unwrap();
    let mut file = File::open(dec_config.in_name).unwrap();
    let mut br = FileReader::new_read(&mut file);
    let mut dmx = create_demuxer(dmx_f, &mut br).unwrap();

    let mux_f = enc_config.mux_reg.find_muxer(enc_config.muxer).unwrap();
    let out_name = "assets/test_out/".to_owned() + enc_config.out_name;
    let file = File::create(&out_name).unwrap();
    let mut bw = FileWriter::new_write(file);
    let mut out_sm = StreamManager::new();
    let mux_caps = mux_f.get_capabilities();
    let mut stream_map: Vec<Option<usize>> = Vec::new();
    let mut has_video = false;
    let mut has_audio = false;
    for stream in dmx.get_streams() {
        let mut copy_stream = false;
        match mux_caps {
            MuxerCapabilities::SingleVideo(_) | MuxerCapabilities::OnlyVideo => {
                copy_stream = stream.get_media_type() == StreamType::Video;
                has_video = true;
            },
            MuxerCapabilities::SingleAudio(_) | MuxerCapabilities::OnlyAudio => {
                copy_stream = stream.get_media_type() == StreamType::Audio;
                has_audio = true;
            },
            MuxerCapabilities::SingleVideoAndAudio(_, _) => {
                if stream.get_media_type() == StreamType::Video {
                    copy_stream = !has_video;
                    has_video = true;
                }
                if stream.get_media_type() == StreamType::Audio {
                    copy_stream = !has_audio;
                    has_audio = true;
                }
            },
            MuxerCapabilities::Universal => {
                if stream.get_media_type() == StreamType::Video {
                    copy_stream = true;
                    has_video = true;
                }
                if stream.get_media_type() == StreamType::Audio {
                    copy_stream = true;
                    has_audio = true;
                }
            }
        };
        if copy_stream {
            let streamno = out_sm.add_stream(NAStream::clone(&stream)).unwrap();
            stream_map.push(Some(streamno));
            match mux_caps {
                MuxerCapabilities::SingleVideo(_) | MuxerCapabilities::SingleAudio(_) => break,
                _ => {},
            };
        } else {
            stream_map.push(None);
        }
    }
    assert!(out_sm.get_num_streams() > 0);
    let mut mux = create_muxer(mux_f, out_sm, &mut bw).unwrap();

    loop {
        let pktres = dmx.get_frame();
        if let Err(e) = pktres {
            if e == DemuxerError::EOF { break; }
            panic!("error");
        }
        let mut pkt = pktres.unwrap();
        println!("Got {}", pkt);
        if let Some(new_id) = stream_map[pkt.get_stream().id as usize] {
            pkt.reassign(mux.get_stream(new_id).unwrap(), pkt.get_time_information());
            mux.mux_frame(pkt).unwrap();
        }
    }

    mux.end().unwrap();
}

/// Tests muxer by making it mux raw streams from the input file and comparing MD5 hash of the result to the provided one.
///
/// Streams not fitting the output profile (e.g. a video stream or a second audio stream for WAV muxer) will be ignored.
pub fn test_remuxing_md5(dec_config: &DecoderTestParams, muxer: &str, mux_reg: &RegisteredMuxers, md5_hash: [u32; 4]) {
    let dmx_f = dec_config.dmx_reg.find_demuxer(dec_config.demuxer).unwrap();
    let mut file = File::open(dec_config.in_name).unwrap();
    let mut br = FileReader::new_read(&mut file);
    let mut dmx = create_demuxer(dmx_f, &mut br).unwrap();

    let mux_f = mux_reg.find_muxer(muxer).unwrap();

    let mut dst = Vec::with_capacity(1 << 10);
    let mut bw = GrowableMemoryWriter::new_write(&mut dst);
    let mut out_sm = StreamManager::new();
    let mux_caps = mux_f.get_capabilities();
    let mut stream_map: Vec<Option<usize>> = Vec::new();
    let mut has_video = false;
    let mut has_audio = false;
    for stream in dmx.get_streams() {
        let mut copy_stream = false;
        match mux_caps {
            MuxerCapabilities::SingleVideo(_) | MuxerCapabilities::OnlyVideo => {
                copy_stream = stream.get_media_type() == StreamType::Video;
                has_video = true;
            },
            MuxerCapabilities::SingleAudio(_) | MuxerCapabilities::OnlyAudio => {
                copy_stream = stream.get_media_type() == StreamType::Audio;
                has_audio = true;
            },
            MuxerCapabilities::SingleVideoAndAudio(_, _) => {
                if stream.get_media_type() == StreamType::Video {
                    copy_stream = !has_video;
                    has_video = true;
                }
                if stream.get_media_type() == StreamType::Audio {
                    copy_stream = !has_audio;
                    has_audio = true;
                }
            },
            MuxerCapabilities::Universal => {
                if stream.get_media_type() == StreamType::Video {
                    copy_stream = true;
                    has_video = true;
                }
                if stream.get_media_type() == StreamType::Audio {
                    copy_stream = true;
                    has_audio = true;
                }
            }
        };
        if copy_stream {
            let streamno = out_sm.add_stream(NAStream::clone(&stream)).unwrap();
            stream_map.push(Some(streamno));
            match mux_caps {
                MuxerCapabilities::SingleVideo(_) | MuxerCapabilities::SingleAudio(_) => break,
                _ => {},
            };
        } else {
            stream_map.push(None);
        }
    }
    assert!(out_sm.get_num_streams() > 0);
    let mut mux = create_muxer(mux_f, out_sm, &mut bw).unwrap();

    loop {
        let pktres = dmx.get_frame();
        if let Err(e) = pktres {
            if e == DemuxerError::EOF { break; }
            panic!("error");
        }
        let mut pkt = pktres.unwrap();
        println!("Got {}", pkt);
        if let Some(new_id) = stream_map[pkt.get_stream().id as usize] {
            pkt.reassign(mux.get_stream(new_id).unwrap(), pkt.get_time_information());
            mux.mux_frame(pkt).unwrap();
        }
    }

    mux.end().unwrap();

    let mut hash = [0; 4];
    MD5::calculate_hash(dst.as_slice(), &mut hash);
    println!("output hash {:08x}{:08x}{:08x}{:08x}", hash[0], hash[1], hash[2], hash[3]);
    assert_eq!(hash, md5_hash);
}

/// Tests an encoder by decoding a stream from input file, feeding it to the encoder and muxing the result into output file.
pub fn test_encoding_to_file(dec_config: &DecoderTestParams, enc_config: &EncoderTestParams, mut enc_params: EncodeParameters, enc_options: &[NAOption]) {
    let dmx_f = dec_config.dmx_reg.find_demuxer(dec_config.demuxer).unwrap();
    let mut file = File::open(dec_config.in_name).unwrap();
    let mut br = FileReader::new_read(&mut file);
    let mut dmx = create_demuxer(dmx_f, &mut br).unwrap();

    let in_stream = dmx.get_streams().find(|strm| strm.get_media_type() == dec_config.stream_type).unwrap();
    let in_stream_id = in_stream.id;
    let decfunc = dec_config.dec_reg.find_decoder(in_stream.get_info().get_name()).unwrap();
    let mut dec = (decfunc)();
    let mut dsupp = Box::new(NADecoderSupport::new());
    dec.init(&mut dsupp, in_stream.get_info()).unwrap();

    let mut out_sm = StreamManager::new();
    enc_params.tb_num = in_stream.tb_num;
    enc_params.tb_den = in_stream.tb_den;

    if let (NACodecTypeInfo::Video(ref mut vinfo), Some(ref_vinfo)) = (&mut enc_params.format, in_stream.get_info().get_properties().get_video_info()) {
        if vinfo.width == 0 {
            vinfo.width  = ref_vinfo.width;
            vinfo.height = ref_vinfo.height;
        }
    }
    let mut dst_chmap = NAChannelMap::new();
    if let (NACodecTypeInfo::Audio(ref mut ainfo), Some(ref_ainfo)) = (&mut enc_params.format, in_stream.get_info().get_properties().get_audio_info()) {
        if ainfo.sample_rate == 0 {
            ainfo.sample_rate = ref_ainfo.sample_rate;
        }
        if ainfo.channels == 0 {
            ainfo.channels = ref_ainfo.channels;
        }
        match ainfo.channels {
            1 => {
                dst_chmap.add_channel(NAChannelType::C);
            },
            2 => {
                dst_chmap.add_channel(NAChannelType::L);
                dst_chmap.add_channel(NAChannelType::R);
            },
            _ => panic!("cannot guess channel map"),
        }
    }

    let encfunc = enc_config.enc_reg.find_encoder(enc_config.enc_name).unwrap();
    let mut encoder = (encfunc)();
    encoder.set_options(enc_options);
    let out_str = encoder.init(0, enc_params).unwrap();
    out_sm.add_stream(NAStream::clone(&out_str));

    let mux_f = enc_config.mux_reg.find_muxer(enc_config.muxer).unwrap();
    let out_name = "assets/test_out/".to_owned() + enc_config.out_name;
    let file = File::create(&out_name).unwrap();
    let mut bw = FileWriter::new_write(file);
    let mut mux = create_muxer(mux_f, out_sm, &mut bw).unwrap();

    let (mut ifmt, dst_vinfo) = if let NACodecTypeInfo::Video(vinfo) = enc_params.format {
            (ScaleInfo { fmt: vinfo.format, width: vinfo.width, height: vinfo.height },
             vinfo)
        } else {
            (ScaleInfo { fmt: YUV420_FORMAT, width: 2, height: 2 },
             NAVideoInfo { width: 2, height: 2, format: YUV420_FORMAT, flipped: false, bits: 12 })
        };
    let ofmt = ifmt;
    let mut scaler = NAScale::new(ifmt, ofmt).unwrap();
    let mut cvt_buf = alloc_video_buffer(dst_vinfo, 2).unwrap();
    loop {
        let pktres = dmx.get_frame();
        if let Err(e) = pktres {
            if e == DemuxerError::EOF { break; }
            panic!("decoding error");
        }
        let pkt = pktres.unwrap();
        if pkt.get_stream().id != in_stream_id { continue; }
        let frm = dec.decode(&mut dsupp, &pkt).unwrap();
        let buf = frm.get_buffer();
        let cfrm = if let NACodecTypeInfo::Video(_) = enc_params.format {
                let cur_ifmt = get_scale_fmt_from_pic(&buf);
                if cur_ifmt != ifmt {
                    ifmt = cur_ifmt;
                    scaler = NAScale::new(ifmt, ofmt).unwrap();
                }
                scaler.convert(&buf, &mut cvt_buf).unwrap();
                NAFrame::new(frm.get_time_information(), frm.frame_type, frm.key, frm.get_info(), cvt_buf.clone())
            } else if let NACodecTypeInfo::Audio(ref dst_ainfo) = enc_params.format {
                let cvt_buf = convert_audio_frame(&buf, dst_ainfo, &dst_chmap).unwrap();
                NAFrame::new(frm.get_time_information(), frm.frame_type, frm.key, frm.get_info(), cvt_buf)
            } else {
                panic!("unexpected format");
            };
        encoder.encode(&cfrm).unwrap();
        while let Ok(Some(pkt)) = encoder.get_packet() {
            mux.mux_frame(pkt).unwrap();
        }
        if let Some(maxts) = dec_config.limit {
            if frm.get_pts().unwrap_or(0) >= maxts {
                break;
            }
        }
    }
    encoder.flush().unwrap();
    while let Ok(Some(pkt)) = encoder.get_packet() {
        mux.mux_frame(pkt).unwrap();
    }
    mux.end().unwrap();
}

/// Tests an encoder by decoding a stream from input file, feeding it to the encoder and calculating the hash of codec information and packet data.
pub fn test_encoding_md5(dec_config: &DecoderTestParams, enc_config: &EncoderTestParams, mut enc_params: EncodeParameters, enc_options: &[NAOption], ref_hash: &[u32; 4]) {
    let dmx_f = dec_config.dmx_reg.find_demuxer(dec_config.demuxer).unwrap();
    let mut file = File::open(dec_config.in_name).unwrap();
    let mut br = FileReader::new_read(&mut file);
    let mut dmx = create_demuxer(dmx_f, &mut br).unwrap();

    let in_stream = dmx.get_streams().find(|strm| strm.get_media_type() == dec_config.stream_type).unwrap();
    let in_stream_id = in_stream.id;
    let decfunc = dec_config.dec_reg.find_decoder(in_stream.get_info().get_name()).unwrap();
    let mut dec = (decfunc)();
    let mut dsupp = Box::new(NADecoderSupport::new());
    dec.init(&mut dsupp, in_stream.get_info()).unwrap();

    enc_params.tb_num = in_stream.tb_num;
    enc_params.tb_den = in_stream.tb_den;

    if let (NACodecTypeInfo::Video(ref mut vinfo), Some(ref_vinfo)) = (&mut enc_params.format, in_stream.get_info().get_properties().get_video_info()) {
        if vinfo.width == 0 {
            vinfo.width  = ref_vinfo.width;
            vinfo.height = ref_vinfo.height;
        }
    }
    let mut dst_chmap = NAChannelMap::new();
    if let (NACodecTypeInfo::Audio(ref mut ainfo), Some(ref_ainfo)) = (&mut enc_params.format, in_stream.get_info().get_properties().get_audio_info()) {
        if ainfo.sample_rate == 0 {
            ainfo.sample_rate = ref_ainfo.sample_rate;
        }
        if ainfo.channels == 0 {
            ainfo.channels = ref_ainfo.channels;
        }
        match ainfo.channels {
            1 => {
                dst_chmap.add_channel(NAChannelType::C);
            },
            2 => {
                dst_chmap.add_channel(NAChannelType::L);
                dst_chmap.add_channel(NAChannelType::R);
            },
            _ => panic!("cannot guess channel map"),
        }
    }

    let encfunc = enc_config.enc_reg.find_encoder(enc_config.enc_name).unwrap();
    let mut encoder = (encfunc)();
    encoder.set_options(enc_options);
    let out_str = encoder.init(0, enc_params).unwrap();

    let mut md5 = MD5::new();
    let info = out_str.get_info();
    md5.update_hash(info.get_name().as_bytes());
    match info.get_properties() {
        NACodecTypeInfo::Video(ref vinfo) => {
            let mut hdr = [0u8; 10];
            hdr[0] = (vinfo.width  >> 24) as u8;
            hdr[1] = (vinfo.width  >> 16) as u8;
            hdr[2] = (vinfo.width  >>  8) as u8;
            hdr[3] =  vinfo.width         as u8;
            hdr[4] = (vinfo.height >> 24) as u8;
            hdr[5] = (vinfo.height >> 16) as u8;
            hdr[6] = (vinfo.height >>  8) as u8;
            hdr[7] =  vinfo.height        as u8;
            hdr[8] = vinfo.flipped as u8;
            hdr[9] = vinfo.bits;
            md5.update_hash(&hdr);
        },
        NACodecTypeInfo::Audio(ref ainfo) => {
            let mut hdr = [0u8; 10];
            hdr[0] = (ainfo.sample_rate >> 24) as u8;
            hdr[1] = (ainfo.sample_rate >> 16) as u8;
            hdr[2] = (ainfo.sample_rate >>  8) as u8;
            hdr[3] =  ainfo.sample_rate        as u8;
            hdr[4] = ainfo.channels;
            hdr[5] = ainfo.format.bits;
            hdr[6] = (ainfo.block_len >> 24) as u8;
            hdr[7] = (ainfo.block_len >> 16) as u8;
            hdr[8] = (ainfo.block_len >>  8) as u8;
            hdr[9] =  ainfo.block_len        as u8;
            md5.update_hash(&hdr);
        },
        _ => {},
    };
    if let Some(ref buf) = info.get_extradata() {
        md5.update_hash(buf.as_slice());
    }

    let (mut ifmt, dst_vinfo) = if let NACodecTypeInfo::Video(vinfo) = enc_params.format {
            (ScaleInfo { fmt: vinfo.format, width: vinfo.width, height: vinfo.height },
             vinfo)
        } else {
            (ScaleInfo { fmt: YUV420_FORMAT, width: 2, height: 2 },
             NAVideoInfo { width: 2, height: 2, format: YUV420_FORMAT, flipped: false, bits: 12 })
        };
    let ofmt = ifmt;
    let mut scaler = NAScale::new(ifmt, ofmt).unwrap();
    let mut cvt_buf = alloc_video_buffer(dst_vinfo, 2).unwrap();
    loop {
        let pktres = dmx.get_frame();
        if let Err(e) = pktres {
            if e == DemuxerError::EOF { break; }
            panic!("decoding error");
        }
        let pkt = pktres.unwrap();
        if pkt.get_stream().id != in_stream_id { continue; }
        let frm = dec.decode(&mut dsupp, &pkt).unwrap();
        let buf = frm.get_buffer();
        let cfrm = if let NACodecTypeInfo::Video(_) = enc_params.format {
                let cur_ifmt = get_scale_fmt_from_pic(&buf);
                if cur_ifmt != ifmt {
                    ifmt = cur_ifmt;
                    scaler = NAScale::new(ifmt, ofmt).unwrap();
                }
                scaler.convert(&buf, &mut cvt_buf).unwrap();
                NAFrame::new(frm.get_time_information(), frm.frame_type, frm.key, frm.get_info(), cvt_buf.clone())
            } else if let NACodecTypeInfo::Audio(ref dst_ainfo) = enc_params.format {
                let cvt_buf = convert_audio_frame(&buf, dst_ainfo, &dst_chmap).unwrap();
                NAFrame::new(frm.get_time_information(), frm.frame_type, frm.key, frm.get_info(), cvt_buf)
            } else {
                panic!("unexpected format");
            };
        encoder.encode(&cfrm).unwrap();
        while let Ok(Some(pkt)) = encoder.get_packet() {
            md5.update_hash(pkt.get_buffer().as_slice());
        }
        if let Some(maxts) = dec_config.limit {
            if frm.get_pts().unwrap_or(0) >= maxts {
                break;
            }
        }
    }
    encoder.flush().unwrap();
    while let Ok(Some(pkt)) = encoder.get_packet() {
        md5.update_hash(pkt.get_buffer().as_slice());
    }

    let mut hash = [0; 4];
    md5.finish();
    md5.get_hash(&mut hash);
    println!("encode hash {}", md5);
    assert_eq!(&hash, ref_hash);
}
