use nihav_core::codecs::*;
use nihav_core::io::byteio::*;
use nihav_core::io::bitreader::*;
use nihav_core::io::codebook::*;
use std::str::FromStr;

const PAL_SIZE: usize = 768;
const SMK_FLAG_INTERLACED: u32 = 0x02;
const SMK_FLAG_SCALED: u32 = 0x04;

struct SmackerTree8 {
    cb:     Option<Codebook<u8>>,
    defsym: u8,
}

fn get_tree8(br: &mut BitReader, bits: &mut [u8; 256], codes: &mut [u32; 256], syms: &mut [u8; 256], count: &mut usize, len: u8, prefix: u32) -> DecoderResult<()> {
    if !br.read_bool()? {
        bits[*count]  = len;
        codes[*count] = prefix;
        syms[*count]                            = br.read(8)? as u8;
        *count += 1;
    } else {
        validate!((*count <= 256 - 2) && (len <= 31));
        get_tree8(br, bits, codes, syms, count, len + 1, prefix)?;
        get_tree8(br, bits, codes, syms, count, len + 1, prefix | (1 << len))?;
    }
    Ok(())
}

pub struct FullTableCodebookDescReader<'a> {
    bits:       &'a [u8],
    codes:      &'a [u32],
    syms:       &'a [u8],
}

impl<'a> FullTableCodebookDescReader<'a> {
    pub fn new(codes: &'a [u32], bits: &'a [u8], syms: &'a [u8]) -> Self {
        Self { bits, codes, syms }
    }
}
impl<'a> CodebookDescReader<u8> for FullTableCodebookDescReader<'a>
{
    fn bits(&mut self, idx: usize) -> u8  { self.bits[idx] }
    fn code(&mut self, idx: usize) -> u32 { self.codes[idx] }
    fn sym (&mut self, idx: usize) -> u8  { self.syms[idx] }
    fn len(&mut self) -> usize { self.bits.len() }
}

impl SmackerTree8 {
    fn new() -> Self {
        Self {
            cb:     None,
            defsym: 0,
        }
    }
    fn decode(&mut self, br: &mut BitReader) -> DecoderResult<()> {
        if !br.read_bool()? { return Ok(()); }

        let mut bits: [u8; 256] = [0; 256];
        let mut codes: [u32; 256] = [0; 256];
        let mut syms: [u8; 256] = [0; 256];
        let mut count = 0;

        get_tree8(br, &mut bits, &mut codes, &mut syms, &mut count, 0, 0)?;
        validate!(!br.read_bool()?);

        if count > 1 {
            let mut cr = FullTableCodebookDescReader::new(&codes[0..count], &bits[0..count], &syms[0..count]);
            let cb = Codebook::new(&mut cr, CodebookMode::LSB)?;
            self.cb = Some(cb);
        } else {
            self.defsym = syms[0];
        }

        Ok(())
    }
}

struct SmackerTree16 {
    tree:   Vec<u32>,
    last:   [usize; 3],
}

struct SmackerTree16Builder {
    tree_lo:    SmackerTree8,
    tree_hi:    SmackerTree8,
    nsyms:      usize,
    esc:        [u32; 3],
}

const SMK_BIGTREE_NODE: u32 = 0x80000000;
const SMK_LAST_UNINIT: usize = 0xFFFFFFFF;

impl SmackerTree16Builder {
    fn get_tree16(&mut self, br: &mut BitReader, tree: &mut SmackerTree16, depth: usize) -> DecoderResult<u32> {
        validate!(tree.tree.len() < self.nsyms);
        validate!(depth <= 32);
        if !br.read_bool()? {
            let lo = br.read_tree8(&self.tree_lo)?;
            let hi = br.read_tree8(&self.tree_hi)?;
            let mut sym = (((hi as u16) << 8) | (lo as u16)) as u32;
            if sym == self.esc[0] {
                tree.last[0] = tree.tree.len();
                sym = 0;
            } else if sym == self.esc[1] {
                tree.last[1] = tree.tree.len();
                sym = 0;
            } else if sym == self.esc[2] {
                tree.last[2] = tree.tree.len();
                sym = 0;
            }
            tree.tree.push(sym);
            Ok(1)
        } else {
            let cur_idx = tree.tree.len();
            tree.tree.push(0);
            let lcount = self.get_tree16(br, tree, depth + 1)?;
            let rcount = self.get_tree16(br, tree, depth + 1)?;
            tree.tree[cur_idx] = SMK_BIGTREE_NODE | lcount;
            Ok(lcount + rcount + 1)
        }
    }
}

impl SmackerTree16 {
    fn new() -> Self {
        Self {
            tree:   Vec::new(),
            last:   [SMK_LAST_UNINIT; 3],
        }
    }
    fn decode(&mut self, br: &mut BitReader, size: u32) -> DecoderResult<()> {
        if !br.read_bool()? { return Ok(()); }

        let mut tree_lo = SmackerTree8::new();
        tree_lo.decode(br)?;
        let mut tree_hi = SmackerTree8::new();
        tree_hi.decode(br)?;

        let mut esc: [u32; 3] = [0; 3];
        for esc in esc.iter_mut() {
            *esc = br.read(16)?;
        }

        let nsyms = (((size + 3) >> 2) + 4) as usize;
        self.tree = Vec::with_capacity(nsyms);

        let mut tb = SmackerTree16Builder { tree_lo, tree_hi, nsyms, esc };

        tb.get_tree16(br, self, 0)?;
        validate!(!br.read_bool()?);

        for i in 0..self.last.len() {
            if self.last[i] == SMK_LAST_UNINIT {
                self.last[i] = self.tree.len();
                self.tree.push(0);
            }
        }
        validate!(self.tree.len() <= nsyms);
        Ok(())
    }
    fn reset(&mut self) {
        for i in 0..self.last.len() {
            if self.last[i] != SMK_LAST_UNINIT {
                self.tree[self.last[i]] = 0;
            }
        }
    }
}

trait ReadTree {
    fn read_tree8(&mut self, tree: &SmackerTree8) -> DecoderResult<u8>;
    fn read_bigtree(&mut self, tree: &mut SmackerTree16) -> DecoderResult<u16>;
}

impl<'a> ReadTree for BitReader<'a> {
    fn read_tree8(&mut self, tree: &SmackerTree8) -> DecoderResult<u8> {
        if let Some(ref cb) = tree.cb {
            Ok(self.read_cb(cb)?)
        } else {
            Ok(tree.defsym)
        }
    }
    fn read_bigtree(&mut self, tree: &mut SmackerTree16) -> DecoderResult<u16> {
        let mut pos = 0;
        while (tree.tree[pos] & SMK_BIGTREE_NODE) != 0 {
            if self.read_bool()? {
                pos += (tree.tree[pos] & !SMK_BIGTREE_NODE) as usize;
            }
            pos += 1;
        }
        let val = tree.tree[pos];
        if val != tree.tree[tree.last[0]] {
            tree.tree[tree.last[2]] = tree.tree[tree.last[1]];
            tree.tree[tree.last[1]] = tree.tree[tree.last[0]];
            tree.tree[tree.last[0]] = val;
        }
        Ok(val as u16)
    }
}

const SMK_BLOCK_RUNS: [usize; 64] = [
      1,    2,    3,    4,    5,    6,    7,    8,
      9,   10,   11,   12,   13,   14,   15,   16,
     17,   18,   19,   20,   21,   22,   23,   24,
     25,   26,   27,   28,   29,   30,   31,   32,
     33,   34,   35,   36,   37,   38,   39,   40,
     41,   42,   43,   44,   45,   46,   47,   48,
     49,   50,   51,   52,   53,   54,   55,   56,
     57,   58,   59,  128,  256,  512, 1024, 2048
];

struct SmackerVideoDecoder {
    info:       NACodecInfoRef,
    mmap_tree:  SmackerTree16,
    mclr_tree:  SmackerTree16,
    full_tree:  SmackerTree16,
    type_tree:  SmackerTree16,
    w:          usize,
    h:          usize,
    bw:         usize,
    bh:         usize,
    is_ver4:    bool,
    flags:      u32,
    frame:      Vec<u8>,
    stride:     usize,
}

impl SmackerVideoDecoder {
    fn new() -> Self {
        Self {
            info:       NACodecInfoRef::default(),
            mmap_tree:  SmackerTree16::new(),
            mclr_tree:  SmackerTree16::new(),
            full_tree:  SmackerTree16::new(),
            type_tree:  SmackerTree16::new(),
            w:          0,
            h:          0,
            bw:         0,
            bh:         0,
            is_ver4:    false,
            flags:      0,
            frame:      Vec::new(),
            stride:     0,
        }
    }
    fn block_pos(&self, blk_no: usize) -> usize {
        let bx = blk_no % self.bw;
        let by = blk_no / self.bw;
        bx * 4 + by * 4 * self.stride
    }
    #[allow(clippy::identity_op)]
    fn decode_frame(&mut self, br: &mut BitReader) -> DecoderResult<bool> {
        let mut is_intra = true;
        let blocks = self.bw * self.bh;

        self.mmap_tree.reset();
        self.mclr_tree.reset();
        self.full_tree.reset();
        self.type_tree.reset();

        let mut block = 0;
        while block < blocks {
            let btype = br.read_bigtree(&mut self.type_tree)?;
            let run = SMK_BLOCK_RUNS[((btype as usize) >> 2) & 0x3F];
            validate!(run <= blocks - block);
            match btype & 3 {
                0 => { // two-colour pattern
                        for i in 0..run {
                            let clr = br.read_bigtree(&mut self.mclr_tree)?;
                            let mut map = br.read_bigtree(&mut self.mmap_tree)?;
                            let hi = (clr >> 8) as u8;
                            let lo = (clr & 0xFF) as u8;
                            let mut doff = self.block_pos(block + i);
                            for _ in 0..4 {
                                self.frame[doff + 0] = if (map & 1) != 0 { hi } else { lo };
                                self.frame[doff + 1] = if (map & 2) != 0 { hi } else { lo };
                                self.frame[doff + 2] = if (map & 4) != 0 { hi } else { lo };
                                self.frame[doff + 3] = if (map & 8) != 0 { hi } else { lo };
                                map >>= 4;
                                doff += self.stride;
                            }
                        }
                    },
                1 => { // full
                        let mode = if !self.is_ver4 {
                                0
                            } else if br.read_bool()? {
                                1
                            } else if br.read_bool()? {
                                2
                            } else {
                                0
                            };
                        for i in 0..run {
                            let mut doff = self.block_pos(block + i);
                            match mode {
                                0 => {
                                        for _ in 0..4 {
                                            let clr0 = br.read_bigtree(&mut self.full_tree)?;
                                            let clr1 = br.read_bigtree(&mut self.full_tree)?;
                                            self.frame[doff + 0] = (clr1 & 0xFF) as u8;
                                            self.frame[doff + 1] = (clr1 >> 8) as u8;
                                            self.frame[doff + 2] = (clr0 & 0xFF) as u8;
                                            self.frame[doff + 3] = (clr0 >> 8) as u8;
                                            doff += self.stride;
                                        }
                                    },
                                1 => {
                                        for _ in 0..2 {
                                            let clr = br.read_bigtree(&mut self.full_tree)?;
                                            self.frame[doff + 0] = (clr & 0xFF) as u8;
                                            self.frame[doff + 1] = (clr & 0xFF) as u8;
                                            self.frame[doff + 2] = (clr >> 8) as u8;
                                            self.frame[doff + 3] = (clr >> 8) as u8;
                                            doff += self.stride;
                                            self.frame[doff + 0] = (clr & 0xFF) as u8;
                                            self.frame[doff + 1] = (clr & 0xFF) as u8;
                                            self.frame[doff + 2] = (clr >> 8) as u8;
                                            self.frame[doff + 3] = (clr >> 8) as u8;
                                            doff += self.stride;
                                        }
                                    },
                                2 => {
                                        for _ in 0..2 {
                                            let clr0 = br.read_bigtree(&mut self.full_tree)?;
                                            let clr1 = br.read_bigtree(&mut self.full_tree)?;
                                            self.frame[doff + 0] = (clr1 & 0xFF) as u8;
                                            self.frame[doff + 1] = (clr1 >> 8) as u8;
                                            self.frame[doff + 2] = (clr0 & 0xFF) as u8;
                                            self.frame[doff + 3] = (clr0 >> 8) as u8;
                                            doff += self.stride;
                                            self.frame[doff + 0] = (clr1 & 0xFF) as u8;
                                            self.frame[doff + 1] = (clr1 >> 8) as u8;
                                            self.frame[doff + 2] = (clr0 & 0xFF) as u8;
                                            self.frame[doff + 3] = (clr0 >> 8) as u8;
                                            doff += self.stride;
                                        }
                                    },
                                _ => unreachable!(),
                            };
                        }
                    },
                2 => { // skip
                        is_intra = false;
                    },
                3 => { // fill
                        let clr = (btype >> 8) as u8;
                        for i in 0..run {
                            let mut doff = self.block_pos(block + i);
                            for _ in 0..4 {
                                self.frame[doff + 0] = clr;
                                self.frame[doff + 1] = clr;
                                self.frame[doff + 2] = clr;
                                self.frame[doff + 3] = clr;
                                doff += self.stride;
                            }
                        }
                    },
                _ => unreachable!(),
            };
            block += run;
        }
        Ok(is_intra)
    }
    fn output_frame(&self, buf: &mut NAVideoBuffer<u8>) {
        let stride = buf.get_stride(0);
        let data = buf.get_data_mut().unwrap();
        let dst = data.as_mut_slice();
        let is_scaled = (self.flags & SMK_FLAG_SCALED) != 0;
        let is_interlaced = (self.flags & SMK_FLAG_INTERLACED) != 0;
        let mut didx = 0;
        let mut sidx = 0;
        for _ in 0..self.h {
            dst[didx..][..self.w].copy_from_slice(&self.frame[sidx..][..self.w]);
            sidx += self.stride;
            didx += stride;
            if is_scaled {
                for x in 0..self.w { dst[didx + x] = dst[didx - stride + x]; }
                didx += stride;
            }
            if is_interlaced {
                for x in 0..self.w { dst[didx + x] = 0; }
                didx += stride;
                if is_scaled {
                    for x in 0..self.w { dst[didx + x] = 0; }
                    didx += stride;
                }
            }
        }
    }
}

impl NADecoder for SmackerVideoDecoder {
    fn init(&mut self, _supp: &mut NADecoderSupport, info: NACodecInfoRef) -> DecoderResult<()> {
        if let NACodecTypeInfo::Video(vinfo) = info.get_properties() {
            let w = vinfo.get_width();
            let h = vinfo.get_height();
            let fmt = PAL8_FORMAT;

            self.w = w;
            self.h = h;
            self.bw = w >> 2;
            self.bh = h >> 2;
            let edata = info.get_extradata().unwrap();
            validate!(edata.len() > 24);

            self.stride = w;
            self.frame.resize(w * h, 0);

            let mut br = MemoryReader::new_read(&edata);
            let magic                   = br.read_tag()?;
            self.flags                  = br.read_u32le()?;
            let mmap_size               = br.read_u32le()?;
            let mclr_size               = br.read_u32le()?;
            let full_size               = br.read_u32le()?;
            let type_size               = br.read_u32le()?;

            self.is_ver4 = &magic == b"SMK4";
            let mut br = BitReader::new(&edata[24..], BitReaderMode::LE);
            self.mmap_tree.decode(&mut br, mmap_size)?;
            self.mclr_tree.decode(&mut br, mclr_size)?;
            self.full_tree.decode(&mut br, full_size)?;
            self.type_tree.decode(&mut br, type_size)?;

            let mut out_h = h;
            if (self.flags & SMK_FLAG_INTERLACED) != 0 {
                out_h <<= 1;
            }
            if (self.flags & SMK_FLAG_SCALED) != 0 {
                out_h <<= 1;
            }
            let myinfo = NACodecTypeInfo::Video(NAVideoInfo::new(w, out_h, false, fmt));
            self.info = NACodecInfo::new_ref(info.get_name(), myinfo, info.get_extradata()).into_ref();

            Ok(())
        } else {
            Err(DecoderError::InvalidData)
        }
    }
    fn decode(&mut self, _supp: &mut NADecoderSupport, pkt: &NAPacket) -> DecoderResult<NAFrameRef> {
        let src = pkt.get_buffer();
        validate!(src.len() >= PAL_SIZE);

        let is_intra;
        let ftype;
        let bufinfo;
        if src.len() > PAL_SIZE {
            let mut br = BitReader::new(&src[PAL_SIZE..], BitReaderMode::LE);

            bufinfo = alloc_video_buffer(self.info.get_properties().get_video_info().unwrap(), 2)?;
            let mut buf = bufinfo.get_vbuf().unwrap();
            is_intra = self.decode_frame(&mut br)?;
            self.output_frame(&mut buf);
            let paloff = buf.get_offset(1);
            let data = buf.get_data_mut().unwrap();
            let dst = data.as_mut_slice();
            let palout = &mut dst[paloff..][..PAL_SIZE];
            palout.copy_from_slice(&src[0..PAL_SIZE]);
            ftype = if is_intra { FrameType::I } else { FrameType::P };
        } else {
            bufinfo  = NABufferType::None;
            ftype    = FrameType::Skip;
            is_intra = false;
        }

        let mut frm = NAFrame::new_from_pkt(pkt, self.info.clone(), bufinfo);
        frm.set_keyframe(is_intra);
        frm.set_frame_type(ftype);
        Ok(frm.into_ref())
    }
    fn flush(&mut self) {
        for el in self.frame.iter_mut() {
            *el = 0;
        }
    }
}

impl NAOptionHandler for SmackerVideoDecoder {
    fn get_supported_options(&self) -> &[NAOptionDefinition] { &[] }
    fn set_options(&mut self, _options: &[NAOption]) { }
    fn query_option_value(&self, _name: &str) -> Option<NAValue> { None }
}

pub fn get_decoder_video() -> Box<dyn NADecoder + Send> {
    Box::new(SmackerVideoDecoder::new())
}

struct SmackerAudioDecoder {
    ainfo:      NAAudioInfo,
    chmap:      NAChannelMap,
    chans:      usize,
    bits:       u8,
}

impl SmackerAudioDecoder {
    fn new() -> Self {
        Self {
            ainfo:  NAAudioInfo::new(0, 1, SND_S16P_FORMAT, 0),
            chmap:  NAChannelMap::new(),
            chans:  0,
            bits:   0,
        }
    }
}

impl NADecoder for SmackerAudioDecoder {
    fn init(&mut self, _supp: &mut NADecoderSupport, info: NACodecInfoRef) -> DecoderResult<()> {
        if let NACodecTypeInfo::Audio(ainfo) = info.get_properties() {
            self.bits = ainfo.get_format().get_bits();
            let fmt = if self.bits == 8 { SND_U8_FORMAT } else { SND_S16_FORMAT };
            self.chans = ainfo.get_channels() as usize;
            self.ainfo = NAAudioInfo::new(ainfo.get_sample_rate(), ainfo.get_channels(), fmt, 0);
            self.chmap = NAChannelMap::from_str(if ainfo.get_channels() == 2 {"L,R"} else {"C"}).unwrap();
            Ok(())
        } else {
            Err(DecoderError::InvalidData)
        }
    }
    #[allow(clippy::identity_op)]
    fn decode(&mut self, _supp: &mut NADecoderSupport, pkt: &NAPacket) -> DecoderResult<NAFrameRef> {
        let info = pkt.get_stream().get_info();
        if let NACodecTypeInfo::Audio(_) = info.get_properties() {
            let src = pkt.get_buffer();
            validate!(src.len() > 4);
            let mut br = BitReader::new(&src, BitReaderMode::LE);
            let unp_size                        = br.read(32)? as usize;
            if !br.read_bool()? {
                let mut frm = NAFrame::new_from_pkt(pkt, info.clone(), NABufferType::None);
                frm.set_frame_type(FrameType::Skip);
                return Ok(frm.into_ref());
            }
            let stereo                          = br.read_bool()?;
            let bits16                          = br.read_bool()?;
            validate!(!(stereo ^ (self.chans == 2)));
            validate!(!(bits16 ^ (self.bits == 16)));

            let abuf;
            let samples;
            let nch = if stereo { 2 } else { 1 };
            if bits16 {
                samples = unp_size / 2 / nch;
                let mask = if stereo { 1 } else { 0 };
                let mut trees: [SmackerTree8; 4] = [SmackerTree8::new(), SmackerTree8::new(), SmackerTree8::new(), SmackerTree8::new()];
                for tree in trees[..nch*2].iter_mut() {
                    tree.decode(&mut br)?;
                }
                let mut pred: [i16; 2] = [0; 2];
                for i in 0..nch {
                    let hi                      = br.read(8)?;
                    let lo                      = br.read(8)?;
                    pred[nch - i - 1] = (lo | (hi << 8)) as i16;
                }

                abuf = alloc_audio_buffer(self.ainfo, samples, self.chmap.clone())?;
                let mut adata = abuf.get_abuf_i16().unwrap();
                let dst = adata.get_data_mut().unwrap();
                dst[..nch].copy_from_slice(&pred[..nch]);
                for (i, out) in dst[nch..(unp_size >> 1)].iter_mut().enumerate() {
                    let idx = i & mask;
                    let lo                      = br.read_tree8(&trees[idx * 2 + 0])? as u16;
                    let hi                      = br.read_tree8(&trees[idx * 2 + 1])? as u16;
                    let diff = (lo | (hi << 8)) as i16;
                    pred[idx] = pred[idx].wrapping_add(diff);
                    *out = pred[idx];
                }
            } else {
                samples = unp_size / nch;
                abuf = alloc_audio_buffer(self.ainfo, samples, self.chmap.clone())?;
                let mut adata = abuf.get_abuf_u8().unwrap();
                let dst = adata.get_data_mut().unwrap();
                if stereo {
                    let mut trees: [SmackerTree8; 2] = [SmackerTree8::new(), SmackerTree8::new()];
                    trees[0].decode(&mut br)?;
                    trees[1].decode(&mut br)?;
                    let pred0                   = br.read(8)? as u8;
                    let pred1                   = br.read(8)? as u8;
                    let mut pred: [u8; 2] = [ pred1, pred0 ];
                    dst[..2].copy_from_slice(&pred);
                    for (i, out) in dst[2..unp_size].iter_mut().enumerate() {
                        let diff = br.read_tree8(&trees[i & 1])?;
                        pred[i & 1] = pred[i & 1].wrapping_add(diff);
                        *out = pred[i & 1];
                    }
                } else {
                    let mut tree = SmackerTree8::new();
                    tree.decode(&mut br)?;
                    let mut pred                = br.read(8)? as u8;
                    dst[0] = pred;
                    for out in dst[1..unp_size].iter_mut() {
                        let diff = br.read_tree8(&tree)?;
                        pred = pred.wrapping_add(diff);
                        *out = pred;
                    }
                }
            }
            let mut frm = NAFrame::new_from_pkt(pkt, info, abuf);
            frm.set_duration(Some(samples as u64));
            frm.set_keyframe(false);
            Ok(frm.into_ref())
        } else {
            Err(DecoderError::InvalidData)
        }
    }
    fn flush(&mut self) {
    }
}

impl NAOptionHandler for SmackerAudioDecoder {
    fn get_supported_options(&self) -> &[NAOptionDefinition] { &[] }
    fn set_options(&mut self, _options: &[NAOption]) { }
    fn query_option_value(&self, _name: &str) -> Option<NAValue> { None }
}

pub fn get_decoder_audio() -> Box<dyn NADecoder + Send> {
    Box::new(SmackerAudioDecoder::new())
}

#[cfg(test)]
mod test {
    use nihav_core::codecs::RegisteredDecoders;
    use nihav_core::demuxers::RegisteredDemuxers;
    use nihav_codec_support::test::dec_video::*;
    use crate::rad_register_all_decoders;
    use crate::rad_register_all_demuxers;
    #[test]
    fn test_smkvid() {
        let mut dmx_reg = RegisteredDemuxers::new();
        rad_register_all_demuxers(&mut dmx_reg);
        let mut dec_reg = RegisteredDecoders::new();
        rad_register_all_decoders(&mut dec_reg);

        // sample from Heroes of Might and Magic 2
        test_decoding("smacker", "smacker-video", "assets/RAD/credits.smk", None, &dmx_reg, &dec_reg,
                      ExpectedTestResult::MD5Frames(vec![
                            [0x0983944a, 0xa23503f8, 0x2602b589, 0x13b53480],
                            [0xb6c2bf1e, 0x2ee5fa60, 0x9896a6dc, 0x760b5737],
                            [0xc7c6d112, 0x2c3c5bac, 0x63684974, 0xa6573b1e],
                            [0x100e2871, 0xbc670db7, 0x54a802e5, 0xb5ba0b07],
                            [0xcd9d22ce, 0x7f195dc9, 0x93c47105, 0x6acf8aa7],
                            [0x84e82fdb, 0x304f24a8, 0x17466d73, 0x20182c33],
                            [0xfcae613f, 0xddab2bd4, 0x9d351ee5, 0x2d0aea24],
                            [0xea32a37c, 0x94d76dda, 0xbb34ca1d, 0xfc9d8a25],
                            [0x37855f28, 0xb508a386, 0x1f0bd981, 0x0f967e25],
                            [0x9b9f453a, 0xf6e34fe7, 0x9279fd71, 0x850a4f36]]));
    }
    #[test]
    fn test_smkvid4() {
        let mut dmx_reg = RegisteredDemuxers::new();
        rad_register_all_demuxers(&mut dmx_reg);
        let mut dec_reg = RegisteredDecoders::new();
        rad_register_all_decoders(&mut dec_reg);

        // sample from Army Man: Air Tactics
        test_decoding("smacker", "smacker-video", "assets/RAD/cine2.smk", Some(32000), &dmx_reg, &dec_reg,
                      ExpectedTestResult::MD5Frames(vec![
                            [0x9da29a47, 0xd61a31ef, 0xa0da6b39, 0xbcf48d01],
                            [0x604be492, 0x347854c0, 0xfeca481d, 0x98161276],
                            [0x3922ad4a, 0x21ac6633, 0xe2c62e0b, 0x11cf3db5],
                            [0xf1e58d8b, 0x539a6e41, 0x9ba5ebd6, 0x4129f62f],
                            [0x549c73f5, 0x5eecbbc4, 0xcf301c7f, 0x7dfad240]]));
    }
    #[test]
    fn test_smkaud_u8() {
        let mut dmx_reg = RegisteredDemuxers::new();
        rad_register_all_demuxers(&mut dmx_reg);
        let mut dec_reg = RegisteredDecoders::new();
        rad_register_all_decoders(&mut dec_reg);

        // sample: https://samples.mplayerhq.hu/game-formats/smacker/wetlands/wetlogo.smk
        test_decoding("smacker", "smacker-audio", "assets/RAD/wetlogo.smk", None, &dmx_reg, &dec_reg,
                      ExpectedTestResult::MD5([0xc686b833, 0x0a203038, 0x012f6d9b, 0xa4186d44]));
    }
    #[test]
    fn test_smkaud_s16() {
        let mut dmx_reg = RegisteredDemuxers::new();
        rad_register_all_demuxers(&mut dmx_reg);
        let mut dec_reg = RegisteredDecoders::new();
        rad_register_all_decoders(&mut dec_reg);

        // sample: https://samples.mplayerhq.hu/game-formats/smacker/20130507_audio-distortion.smk
        test_decoding("smacker", "smacker-audio", "assets/RAD/20130507_audio-distortion.smk", None, &dmx_reg, &dec_reg,
                      ExpectedTestResult::MD5([0x942a0922, 0x182bb5fd, 0x94ab7a59, 0x2028d810]));
    }
}
