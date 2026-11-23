use std::collections::VecDeque;

use nihav_core::codecs::*;
use nihav_core::io::bitwriter::*;
use nihav_codec_support::dsp::dct::*;
use nihav_codec_support::dsp::fft::*;
use super::binkauddata::*;

trait WriteBinkFloat {
    fn write_float(&mut self, val: f32);
}

impl WriteBinkFloat for BitWriter {
    fn write_float(&mut self, val: f32) {
        let bits = val.to_bits();
        let sign = bits >> 31;
        let nexp = ((bits >> 23).wrapping_sub(0x7E)) & 0x1F;
        let mant = bits & ((1 << 23) - 1);
        self.write(nexp, 5);
        self.write(mant, 23);
        self.write(sign, 1);
    }
}

#[derive(Default)]
struct RateControl {
    quality:        u8,
    bitrate:        u32,
    bitpool:        u32,
    spos:           u32,
    srate:          u32,
    blk_size:       u32,
    lambda:         f32,
}

impl RateControl {
    fn init(&mut self, quality: u8, bitrate: u32, srate: u32, blk_size: u32) {
        self.quality  = quality;
        self.bitrate  = bitrate;
        self.bitpool  = bitrate;
        self.srate    = srate;
        self.spos     = 0;
        self.blk_size = blk_size;
        self.lambda   = if self.quality != 0 {
                ((100 - self.quality) as f32) / 20.0
            } else {
                1.0
            };
    }
    fn get_tgt_size(&self) -> usize {
        if self.bitrate > 0 {
            (self.bitpool * self.blk_size / (self.srate - self.spos)) as usize
        } else {
            0
        }
    }
    fn update(&mut self, real_size: usize) {
        if self.bitrate == 0 {
            return;
        }

        let tgt_size = self.get_tgt_size();

        if real_size < tgt_size - tgt_size / 8 {
            self.lambda -= 0.2;
            if self.lambda < 0.0 {
                self.lambda = 0.0;
            }
        }
        if real_size > tgt_size + tgt_size / 8 {
            self.lambda += 0.5;
        }

        self.spos += self.blk_size;
        while self.spos >= self.srate {
            self.spos -= self.srate;
            self.bitpool += self.bitrate;
        }
        self.bitpool = self.bitpool.saturating_sub(real_size as u32);
    }
}

#[derive(Default)]
struct SampleBuffer {
    samples:        Vec<f32>,
    read_pos:       usize,
    frame_len:      usize,
    step_size:      usize,
}

impl SampleBuffer {
    fn reset(&mut self) {
        self.samples.clear();
        self.read_pos = 0;
    }
    fn num_avail(&self) -> usize { self.samples.len() - self.read_pos }
    fn add_mono(&mut self, src: &[f32]) {
        self.samples.extend_from_slice(src);
    }
    fn add_stereo(&mut self, left: &[f32], right: &[f32]) {
        for (&l, &r) in left.iter().zip(right.iter()) {
            self.samples.push(l);
            self.samples.push(r);
        }
    }
    fn norm(&mut self) {
        if self.read_pos == self.samples.len() {
            self.read_pos = 0;
            self.samples.clear();
        } else if self.read_pos * 2 >= self.samples.len() {
            let len = self.num_avail();
            let (head, tail) = self.samples.split_at_mut(self.read_pos);
            head[..tail.len()].copy_from_slice(tail);
            self.read_pos = 0;
            self.samples.truncate(len);
        }
    }
    fn read_frame(&self, dst: &mut [f32]) {
        let src = &self.samples[self.read_pos..];
        let len = dst.len().min(src.len()).min(self.frame_len);
        dst[..len].copy_from_slice(&src[..len]);
        let ovl_len = self.frame_len - self.step_size;
        for (i, sample) in dst[..ovl_len].iter_mut().enumerate() {
            *sample *= (i as f32) / (ovl_len as f32);
        }
        for (i, sample) in dst[self.step_size..][..ovl_len].iter_mut().rev().enumerate() {
            *sample *= (i as f32) / (ovl_len as f32);
        }
    }
    fn consume_frame(&mut self) {
        self.read_pos += self.step_size;
        if self.read_pos > self.samples.len() {
            self.read_pos = self.samples.len();
        }
        self.norm();
    }
}

fn find_quant(src: &[f32], quants: &[f32; 96], lambda: f32) -> usize {
    let maxval = src.iter().fold(0.0f32, |acc, &a| acc.max(a.abs()));

    let mut best = 0;
    let mut best_dist = maxval * maxval * (src.len() as f32);
    for (i, &q) in quants.iter().enumerate() {
        if maxval / q > 32767.0 { continue; }
        if q > maxval * 2.0 { break; }
        let mut dist = 0.0;
        let mut maxqv = 0.0f32;
        let mut signs = 0u32;
        for &el in src.iter() {
            let qval = (el.abs() / q).round();
            let iqval = qval * q;
            dist += (el.abs() - iqval) * (el.abs() - iqval);
            maxqv = maxqv.max(qval);
            if qval > 0.0 {
                signs += 1;
            }
        }
        let bits = if maxqv > 0.0 {
                maxqv.log2().ceil() * (src.len() as f32) + (signs as f32)
            } else { 0.0 };
        let metric = (dist + 1.0).log2() * 10.0 + bits * lambda;
        if metric < best_dist {
            best_dist = metric;
            best = i;
        }
    }
    best
}

enum Transform {
    None,
    DCT(DCT),
    RDFT(RDFT),
}

struct BinkAudioEncoder {
    stream:         Option<NAStreamRef>,
    version:        char,
    stereo:         bool,

    frm_size:       usize,
    frame_bits:     u8,
    overlap_size:   usize,

    apts:           u64,
    use_dct:        bool,
    preload:        usize,

    samples:        SampleBuffer,
    transform:      Transform,
    scale:          f32,
    quants:         [f32; 96],
    num_bands:      usize,
    bands:          [usize; MAX_BANDS + 1],
    first_frame:    bool,
    sample_step:    usize,
    leftover:       usize,

    tmp:            Vec<f32>,

    packets:        VecDeque<NAPacket>,
    rc:             RateControl,

    print_stats:    bool,
    blk_bits:       usize,
    nblks:          usize,
    nsamples:       usize,
    nframes:        usize,
    qstats:         [usize; 96],
    bstats:         [usize; 16],
}

impl BinkAudioEncoder {
    fn new(use_dct: bool) -> Self {
        Self {
            stream:         None,
            version:        'b',
            stereo:         false,

            frm_size:       0,
            frame_bits:     0,
            overlap_size:   0,

            apts:           0,
            use_dct,
            preload:        16,

            samples:        SampleBuffer::default(),
            transform:      Transform::None,
            scale:          0.0,
            quants:         get_quants_table(),
            num_bands:      0,
            bands:          [0; MAX_BANDS + 1],
            first_frame:    true,
            sample_step:    0,
            leftover:       0,

            tmp:            Vec::new(),

            packets:        VecDeque::new(),
            rc:             RateControl::default(),

            print_stats:    false,
            blk_bits:       0,
            nblks:          0,
            nsamples:       0,
            nframes:        0,
            qstats:         [0; 96],
            bstats:         [0; 16],
        }
    }
    #[allow(clippy::transmute_ptr_to_ptr)]
    fn encode_block(&mut self, bw: &mut BitWriter) {
        match self.transform {
            Transform::None => unreachable!(),
            Transform::DCT(ref mut _dct) => unimplemented!(),
            Transform::RDFT(ref mut rdft) => {
                unsafe {
                    let buf = std::mem::transmute::<&mut [f32], &mut [FFTComplex]>(self.tmp.as_mut_slice());
                    rdft.do_rdft_inplace(buf);
                }
            },
        };
        for el in self.tmp.iter_mut() {
            *el *= self.samples.frame_len as f32;
        }
        if self.version == 'b' {
            bw.write(self.tmp[0].to_bits(), 32);
            bw.write(self.tmp[1].to_bits(), 32);
        } else {
            bw.write_float(self.tmp[0]);
            bw.write_float(self.tmp[1]);
        }

        let mut quants = [0; MAX_BANDS];
        for (range, quant) in self.bands.windows(2).take(self.num_bands).zip(quants.iter_mut()) {
            let region = &mut self.tmp[range[0]..range[1]];
            *quant = find_quant(region, &self.quants, self.rc.lambda);
            for el in region.iter_mut() {
                *el /= self.quants[*quant];
            }
        }

        let mut rec_bits = [0u8; 4096];
        for (bval, &coef) in rec_bits.iter_mut().zip(self.tmp.iter()).skip(2) {
            let aval = coef.abs().round() as u32;
            *bval = if aval > 0 { (32 - aval.leading_zeros()) as u8 } else { 0 };
        }

        for &quant in quants[..self.num_bands].iter() {
            self.qstats[quant] += 1;
        }

        for &quant in quants[..self.num_bands].iter() {
            bw.write(quant as u32, 8);
        }
        if self.version == 'b' {
            for (coef_reg, bits_reg) in self.tmp[2..].chunks(16).zip(rec_bits[2..].chunks(16)) {
                let max_bits = bits_reg.iter().fold(0u8, |acc, &a| acc.max(a)).min(15);
                bw.write(u32::from(max_bits), 4);
                self.bstats[usize::from(max_bits)] += coef_reg.len();
                if max_bits != 0 {
                    for &coef in coef_reg.iter() {
                        let bval = (coef.abs().round() as u32).min((1 << max_bits) - 1);
                        bw.write(bval, max_bits);
                        if bval != 0 {
                            bw.write_bit(coef < 0.0);
                        }
                    }
                }
            }
        } else {
            unimplemented!();
        }
    }
    fn encode_packet(&mut self, last: bool) -> EncoderResult<bool> {
        let nsamples = if last {
                self.samples.num_avail()
            } else if self.first_frame {
                self.preload.max(1) * self.frm_size * if self.stereo { 2 } else { 1 }
            } else {
                (self.leftover + self.sample_step) * if self.stereo { 2 } else { 1 }
            };

        let nblocks = if !last {
                let nblk = (nsamples / self.frm_size).max(1);
                if self.samples.num_avail() < nblk * self.frm_size + self.overlap_size {
                    return Ok(false);
                }
                self.leftover = (self.leftover + self.sample_step) % self.frm_size;
                nblk
            } else {
                (nsamples + self.frm_size - 1) / self.frm_size
            };
        let nsamples = if !last { nblocks * self.frm_size } else { nsamples };

        self.nsamples += nsamples >> (self.stereo as u8);
        self.nframes += 1;

        let mut bw = BitWriter::new(Vec::new(), BitWriterMode::LE);
        bw.write((nsamples * 2) as u32, 32); // number of raw audio bytes

        for _nblk in 0..nblocks {
            let start = bw.tell();

            if last {
                for el in self.tmp.iter_mut() {
                    *el = 0.0;
                }
            }
            self.samples.read_frame(&mut self.tmp);
            self.encode_block(&mut bw);
            while (bw.tell() & 0x1F) != 0 {
                bw.write0();
            }
            self.samples.consume_frame();

            let blk_bits = bw.tell() - start;
            self.rc.update(blk_bits);
            self.blk_bits += blk_bits;
            self.nblks += 1;
        }

        if self.first_frame {
            self.first_frame = false;
        }

        let dbuf = bw.end();
        let stream = self.stream.clone().unwrap();
        let (tb_num, tb_den) = stream.get_timebase();
        let ts = NATimeInfo::new(Some(self.apts), None, Some(nsamples as u64), tb_num, tb_den);
        self.apts += (nsamples / if self.stereo { 2 } else { 1 }) as u64;
        self.packets.push_back(NAPacket::new(self.stream.clone().unwrap(), ts, true, dbuf));

        Ok(true)
    }
}

impl NAEncoder for BinkAudioEncoder {
    fn negotiate_format(&self, encinfo: &EncodeParameters) -> EncoderResult<EncodeParameters> {
        match encinfo.format {
            NACodecTypeInfo::None => {
                Ok(EncodeParameters {
                    format: NACodecTypeInfo::Audio(NAAudioInfo::new(0, 1, SND_F32P_FORMAT, 512)),
                    ..Default::default() })
            },
            NACodecTypeInfo::Video(_) => Err(EncoderError::FormatError),
            NACodecTypeInfo::Audio(ainfo) => {
                let mut outinfo = ainfo;
                outinfo.channels = if !self.use_dct { 2 } else { ainfo.channels.clamp(1, 2) };
                outinfo.format = SND_F32P_FORMAT;
                let mut ofmt = *encinfo;
                ofmt.format = NACodecTypeInfo::Audio(outinfo);
                Ok(ofmt)
            }
        }
    }
    fn get_capabilities(&self) -> u64 { ENC_CAPS_CBR }
    fn init(&mut self, stream_id: u32, encinfo: EncodeParameters) -> EncoderResult<NAStreamRef> {
        match encinfo.format {
            NACodecTypeInfo::None => Err(EncoderError::FormatError),
            NACodecTypeInfo::Video(_) => Err(EncoderError::FormatError),
            NACodecTypeInfo::Audio(ainfo) => {
                if ainfo.format != SND_F32P_FORMAT {
                    return Err(EncoderError::FormatError);
                }

                let srate = ainfo.get_sample_rate();
                let mut frame_bits = if srate < 22050 { 9 } else if srate < 44100 { 10 } else { 11 };

                if self.version < 'i' && self.use_dct {
                    println!("DCT is supported starting from version 'i'");
                    return Err(EncoderError::FormatError);
                }

                if !self.use_dct && self.version != 'b' {
                    frame_bits += 1;
                }
                self.frame_bits = frame_bits;
                self.stereo = ainfo.channels == 2;
                let mut duration = (1 << frame_bits) - (1 << (frame_bits - 4));
                let single = !self.use_dct && self.stereo; // RDFT codes samples interleaved as single buffer
                if single {
                    duration >>= 1;
                }

                self.transform = if !self.use_dct {
                        Transform::RDFT(RDFTBuilder::new_rdft(1 << (frame_bits - 1), true, false))
                    } else {
                        Transform::DCT(DCT::new(DCTMode::DCT_II, 1 << frame_bits))
                    };
                self.scale = if !self.use_dct {
                        1.0 / (32768.0 * ((1 << frame_bits) as f32).sqrt())
                    } else {
                        (2.0 / ((1 << frame_bits) as f32)).sqrt() / 1024.0
                    };
                let s_srate = if single { srate } else { srate >> 1 } as usize;
                init_bands(s_srate, 1 << frame_bits, &mut self.num_bands, &mut self.bands);
                self.first_frame = true;

                self.samples.reset();
                self.samples.frame_len = 1 << frame_bits;
                self.samples.step_size = duration * usize::from(ainfo.channels);
                self.frm_size = duration * usize::from(ainfo.channels);
                self.overlap_size = self.frm_size / 15;
                self.tmp = vec![0.0; 1 << frame_bits];

                let blk_len = ((1 << frame_bits) - (1 << (frame_bits - 4))) >> if self.stereo { 1 } else { 0 };
                self.rc.init(encinfo.quality, encinfo.bitrate, srate, blk_len as u32);

                let edata = vec![b'B', b'I', b'K', self.version as u8];

                let out_ainfo = NAAudioInfo::new(ainfo.sample_rate, ainfo.channels, SND_F32P_FORMAT, self.frm_size);
                let name = if !self.use_dct { "bink-audio-rdft" } else { "bink-audio-dct" };
                let info = NACodecInfo::new(name, NACodecTypeInfo::Audio(out_ainfo), Some(edata));
                let mut stream = NAStream::new(StreamType::Audio, stream_id, info, self.frm_size as u32, ainfo.sample_rate, 0);
                stream.set_num(stream_id as usize);
                let stream = stream.into_ref();

                self.stream = Some(stream.clone());
                self.sample_step = (encinfo.tb_num as usize) * (ainfo.sample_rate as usize) / (encinfo.tb_den as usize);

                Ok(stream)
            },
        }
    }
    fn encode(&mut self, frm: &NAFrame) -> EncoderResult<()> {
        if let Some(ref abuf) = frm.get_buffer().get_abuf_f32() {
            let src = abuf.get_data();
            let length = abuf.get_length();

            if abuf.get_info().get_channels() == 2 {
                let offset = abuf.get_offset(1);
                self.samples.add_stereo(&src[..length], &src[offset..][..length]);
            } else {
                self.samples.add_mono(&src[..length]);
            }

            while self.encode_packet(false)? {
            }

            Ok(())
        } else {
            Err(EncoderError::InvalidParameters)
        }
    }
    fn get_packet(&mut self) -> EncoderResult<Option<NAPacket>> {
        Ok(self.packets.pop_front())
    }
    fn flush(&mut self) -> EncoderResult<()> {
        if self.samples.num_avail() > 0 {
            self.encode_packet(true)?;
        }
        Ok(())
    }
}

impl Drop for BinkAudioEncoder {
    fn drop(&mut self) {
        if self.print_stats && self.nblks > 0 {
            println!("encoded {} block(s) in {} frame(s), {} samples total ({}s)",
                    self.nblks, self.nframes, self.nsamples,
                    (self.nsamples as f32) / (self.rc.srate as f32));

            let bitrate = (self.blk_bits as u64) * u64::from(self.rc.srate) / (self.nsamples as u64);
            let br_fmt = if bitrate >= 10_000_000 {
                    format!("{}mbps", bitrate / 1000000)
                } else if bitrate >= 10_000 {
                    format!("{}kbps", bitrate / 1000)
                } else {
                    format!("{}bps", bitrate)
                };
            println!("average bitrate {}", br_fmt);

            let mut end = self.qstats.len();
            for (i, &cnt) in self.qstats.iter().enumerate().rev() {
                if cnt != 0 {
                    break;
                }
                end = i;
            }
            print!("quants used:");
            for &cnt in self.qstats.iter().take(end) {
                print!(" {}", cnt);
            }
            println!();

            let mut end = self.bstats.len();
            for (i, &cnt) in self.bstats.iter().enumerate().rev() {
                if cnt != 0 {
                    break;
                }
                end = i;
            }
            print!("coefficient bits:");
            for &cnt in self.bstats.iter().take(end) {
                print!(" {}", cnt);
            }
            println!();
        }
    }
}

const ENCODER_OPTS: &[NAOptionDefinition] = &[
    NAOptionDefinition {
        name: "version", description: "codec version",
        opt_type: NAOptionDefinitionType::String(Some(&["b", "f", "g", "h", "i", "k"])) },
    NAOptionDefinition {
        name: "preload", description: "number of audio frames to preload",
        opt_type: NAOptionDefinitionType::Int(Some(1), Some(256)) },
    NAOptionDefinition {
        name: "print_stats", description: "print encoder statistics",
        opt_type: NAOptionDefinitionType::Bool },
];

impl NAOptionHandler for BinkAudioEncoder {
    fn get_supported_options(&self) -> &[NAOptionDefinition] { ENCODER_OPTS }
    fn set_options(&mut self, options: &[NAOption]) {
        for option in options.iter() {
            for opt_def in ENCODER_OPTS.iter() {
                if opt_def.check(option).is_ok() {
                    match option.name {
                        "version" => {
                            if let NAValue::String(ref strval) = option.value {
                                match strval.as_str() {
                                    "b" => self.version = 'b',
                                    _ => {
                                        println!("versions beside 'b' are not supported");
                                    },
                                };
                            }
                        },
                        "preload" => {
                            if let NAValue::Int(ival) = option.value {
                                self.preload = ival as usize;
                            }
                        },
                        "print_stats" => {
                            if let NAValue::Bool(bval) = option.value {
                                self.print_stats = bval;
                            }
                        },
                        _ => {},
                    };
                }
            }
        }
    }
    fn query_option_value(&self, name: &str) -> Option<NAValue> {
        match name {
            "version" => Some(NAValue::String(self.version.to_string())),
            "preload" => Some(NAValue::Int(self.preload as i64)),
            "print_stats" => Some(NAValue::Bool(self.print_stats)),
            _ => None,
        }
    }
}

pub fn get_encoder_rdft() -> Box<dyn NAEncoder + Send> { Box::new(BinkAudioEncoder::new(false)) }
#[allow(dead_code)]
pub fn get_encoder_dct() -> Box<dyn NAEncoder + Send> { Box::new(BinkAudioEncoder::new(true)) }

#[cfg(test)]
mod test {
    use nihav_core::codecs::*;
    use nihav_core::demuxers::*;
    use nihav_core::muxers::*;
    use nihav_codec_support::test::enc_video::*;
    use crate::*;
    use nihav_commonfmt::*;

    fn test_encoder(name: &'static str, enc_options: &[NAOption],
                    bitrate: u32, quality: u8, channels: u8, hash: &[u32; 4]) {
        let mut dmx_reg = RegisteredDemuxers::new();
        generic_register_all_demuxers(&mut dmx_reg);
        let mut dec_reg = RegisteredDecoders::new();
        generic_register_all_decoders(&mut dec_reg);
        let mut mux_reg = RegisteredMuxers::new();
        rad_register_all_muxers(&mut mux_reg);
        let mut enc_reg = RegisteredEncoders::new();
        rad_register_all_encoders(&mut enc_reg);

        // sample from a private collection
        let dec_config = DecoderTestParams {
                demuxer:        "wav",
                in_name:        "assets/test-stereo.wav",
                stream_type:    StreamType::Audio,
                limit:          None,//Some(20),
                dmx_reg, dec_reg,
            };
        let enc_config = EncoderTestParams {
                muxer:          "bink",
                enc_name:       "bink-audio-rdft",
                out_name:       name,
                mux_reg, enc_reg,
            };
        let dst_ainfo = NAAudioInfo {
                sample_rate:    0,
                channels,
                format:         SND_F32P_FORMAT,
                block_len:      1024,
            };
        let enc_params = EncodeParameters {
                format:  NACodecTypeInfo::Audio(dst_ainfo),
                quality,
                bitrate: bitrate * 1000,
                tb_num:  0,
                tb_den:  0,
                flags:   0,
            };
        //test_encoding_to_file(&dec_config, &enc_config, enc_params, enc_options);

        test_encoding_md5(&dec_config, &enc_config, enc_params, enc_options, hash);
    }
    #[test]
    fn test_binkaud_encoder_b_q100() {
        let enc_options = &[
            NAOption{name: "version", value: NAValue::String("b".to_owned())},
            //NAOption{name: "print_stats", value: NAValue::Bool(true)},
        ];
        test_encoder("bink-aud-b-q80.bik", enc_options, 0, 100, 2,
            &[0x45c13e44, 0x36ab1efb, 0x84c93f1a, 0x4aa49831]);
    }
    #[test]
    fn test_binkaud_encoder_b_q80() {
        let enc_options = &[
            NAOption{name: "version", value: NAValue::String("b".to_owned())},
            //NAOption{name: "print_stats", value: NAValue::Bool(true)},
        ];
        test_encoder("bink-aud-b-q80.bik", enc_options, 0, 80, 2,
            &[0x3835b6ff, 0xd2da247b, 0xae8ff168, 0x464b4c31]);
    }
    #[test]
    fn test_binkaud_encoder_b_q40() {
        let enc_options = &[
            NAOption{name: "version", value: NAValue::String("b".to_owned())},
            //NAOption{name: "print_stats", value: NAValue::Bool(true)},
        ];
        test_encoder("bink-aud-b-q80.bik", enc_options, 0, 40, 2,
            &[0xe99882b0, 0x12f9be7c, 0xc634c1a7, 0xd88e1c9b]);
    }
    #[test]
    fn test_binkaud_encoder_b_q1() {
        let enc_options = &[
            NAOption{name: "version", value: NAValue::String("b".to_owned())},
            //NAOption{name: "print_stats", value: NAValue::Bool(true)},
        ];
        test_encoder("bink-aud-b-q80.bik", enc_options, 0, 1, 2,
            &[0x783d68e1, 0x9db89348, 0x5348e677, 0x337133fa]);
    }
    #[test]
    fn test_binkaud_encoder_b_br240() {
        let enc_options = &[
            NAOption{name: "version", value: NAValue::String("b".to_owned())},
            //NAOption{name: "print_stats", value: NAValue::Bool(true)},
        ];
        test_encoder("bink-aud-b-180k.bik", enc_options, 240, 0, 2,
            &[0xad33939e, 0x945413f1, 0xf5edc6be, 0xcf8eebd3]);
    }
    #[test]
    fn test_binkaud_encoder_b_br180() {
        let enc_options = &[
            NAOption{name: "version", value: NAValue::String("b".to_owned())},
            //NAOption{name: "print_stats", value: NAValue::Bool(true)},
        ];
        test_encoder("bink-aud-b-180k.bik", enc_options, 180, 0, 2,
            &[0x4d33f1be, 0xb1e662ad, 0x71bd0486, 0x327e053a]);
    }
    #[test]
    fn test_binkaud_encoder_b_br120() {
        let enc_options = &[
            NAOption{name: "version", value: NAValue::String("b".to_owned())},
            //NAOption{name: "print_stats", value: NAValue::Bool(true)},
        ];
        test_encoder("bink-aud-b-120k.bik", enc_options, 120, 0, 2,
            &[0xa1e17945, 0xd837677a, 0x48cd0b3a, 0x3e7c1a03]);
    }
    #[test]
    fn test_binkaud_encoder_b_br60() {
        let enc_options = &[
            NAOption{name: "version", value: NAValue::String("b".to_owned())},
            //NAOption{name: "print_stats", value: NAValue::Bool(true)},
        ];
        test_encoder("bink-aud-b-60k.bik", enc_options, 60, 0, 2,
            &[0xf48cae2e, 0x038ec363, 0x17cb1606, 0x4756f854]);
    }
}
