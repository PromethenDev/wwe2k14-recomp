//! Common routines and tables for JPEG decoding.
use nihav_core::io::bitreader::*;
use nihav_core::io::codebook::*;
use nihav_core::codecs::*;
use crate::codecs::ZIGZAG;

const W1: i32 = 2841;
const W2: i32 = 2676;
const W3: i32 = 2408;
const W5: i32 = 1609;
const W6: i32 = 1108;
const W7: i32 =  565;
const W8: i32 =  181;

const ROW_SHIFT: u8 = 8;
const COL_SHIFT: u8 = 14;

fn idct_row(row: &mut [i16]) {
    let in0 = ((i32::from(row[0])) << 11) + (1 << (ROW_SHIFT - 1));
    let in1 =  (i32::from(row[4])) << 11;
    let in2 =   i32::from(row[6]);
    let in3 =   i32::from(row[2]);
    let in4 =   i32::from(row[1]);
    let in5 =   i32::from(row[7]);
    let in6 =   i32::from(row[5]);
    let in7 =   i32::from(row[3]);

    let tmp = W7 * (in4 + in5);
    let a4 = tmp + (W1 - W7) * in4;
    let a5 = tmp - (W1 + W7) * in5;

    let tmp = W3 * (in6 + in7);
    let a6 = tmp - (W3 - W5) * in6;
    let a7 = tmp - (W3 + W5) * in7;

    let tmp = in0 + in1;

    let a0 = in0 - in1;
    let t1 = W6 * (in2 + in3);
    let a2 = t1 - (W2 + W6) * in2;
    let a3 = t1 + (W2 - W6) * in3;
    let b1 = a4 + a6;

    let b4 = a4 - a6;
    let t2 = a5 - a7;
    let b6 = a5 + a7;
    let b7 = tmp + a3;
    let b5 = tmp - a3;
    let b3 = a0 + a2;
    let b0 = a0 - a2;
    let b2 = (W8 * (b4 + t2) + 128) >> 8;
    let b4 = (W8 * (b4 - t2) + 128) >> 8;

    row[0] = ((b7 + b1) >> ROW_SHIFT) as i16;
    row[7] = ((b7 - b1) >> ROW_SHIFT) as i16;
    row[1] = ((b3 + b2) >> ROW_SHIFT) as i16;
    row[6] = ((b3 - b2) >> ROW_SHIFT) as i16;
    row[2] = ((b0 + b4) >> ROW_SHIFT) as i16;
    row[5] = ((b0 - b4) >> ROW_SHIFT) as i16;
    row[3] = ((b5 + b6) >> ROW_SHIFT) as i16;
    row[4] = ((b5 - b6) >> ROW_SHIFT) as i16;
}

#[allow(clippy::erasing_op)]
#[allow(clippy::identity_op)]
fn idct_col(blk: &mut [i16; 64], off: usize) {
    let in0 = ((i32::from(blk[off + 0*8])) << 8) + (1 << (COL_SHIFT - 1));
    let in1 =  (i32::from(blk[off + 4*8])) << 8;
    let in2 =   i32::from(blk[off + 6*8]);
    let in3 =   i32::from(blk[off + 2*8]);
    let in4 =   i32::from(blk[off + 1*8]);
    let in5 =   i32::from(blk[off + 7*8]);
    let in6 =   i32::from(blk[off + 5*8]);
    let in7 =   i32::from(blk[off + 3*8]);

    let tmp = W7 * (in4 + in5);
    let a4 = (tmp + (W1 - W7) * in4) >> 3;
    let a5 = (tmp - (W1 + W7) * in5) >> 3;

    let tmp = W3 * (in6 + in7);
    let a6 = (tmp - (W3 - W5) * in6) >> 3;
    let a7 = (tmp - (W3 + W5) * in7) >> 3;

    let tmp = in0 + in1;

    let a0 = in0 - in1;
    let t1 = W6 * (in2 + in3);
    let a2 = (t1 - (W2 + W6) * in2) >> 3;
    let a3 = (t1 + (W2 - W6) * in3) >> 3;
    let b1 = a4 + a6;

    let b4 = a4 - a6;
    let t2 = a5 - a7;
    let b6 = a5 + a7;
    let b7 = tmp + a3;
    let b5 = tmp - a3;
    let b3 = a0 + a2;
    let b0 = a0 - a2;
    let b2 = (W8 * (b4 + t2) + 128) >> 8;
    let b4 = (W8 * (b4 - t2) + 128) >> 8;

    blk[off + 0*8] = ((b7 + b1) >> COL_SHIFT) as i16;
    blk[off + 7*8] = ((b7 - b1) >> COL_SHIFT) as i16;
    blk[off + 1*8] = ((b3 + b2) >> COL_SHIFT) as i16;
    blk[off + 6*8] = ((b3 - b2) >> COL_SHIFT) as i16;
    blk[off + 2*8] = ((b0 + b4) >> COL_SHIFT) as i16;
    blk[off + 5*8] = ((b0 - b4) >> COL_SHIFT) as i16;
    blk[off + 3*8] = ((b5 + b6) >> COL_SHIFT) as i16;
    blk[off + 4*8] = ((b5 - b6) >> COL_SHIFT) as i16;
}

/// Performs inverse DCT on the 8x8 coefficients block.
pub fn idct(blk: &mut [i16; 64]) {
    for i in 0..8 { idct_row(&mut blk[i*8..(i+1)*8]); }
    for i in 0..8 { idct_col(blk, i); }
}

/// Puts a single 8x8 block to the destination image.
pub fn put_block(blk: &[i16; 64], dst: &mut [u8], stride: usize) {
    for (drow, srow) in dst.chunks_mut(stride).zip(blk.chunks(8)) {
        for (del, &pix) in drow.iter_mut().zip(srow.iter()) {
            *del = pix.clamp(0, 255) as u8;
        }
    }
}

/// Single component definition.
#[derive(Clone,Copy,Default)]
pub struct ComponentInfo {
    /// Component ID (0 = luma, 1/2 = chroma).
    pub component_id:   usize,
    /// DC codebook ID.
    pub dc_table_id:    usize,
    /// AC codebook ID.
    pub ac_table_id:    usize,
}

/// Known JPEG types.
#[derive(Clone,Copy,Debug,PartialEq)]
pub enum JPEGType {
    None,
    Baseline,
    Extended,
    Progressive,
    Lossless,
    Differential,
    DiffProgressive,
    DiffLossless,
    JPEGLS,
}

struct HuffDescReader<'a> {
    codes:  &'a [u16],
    bits:   &'a [u8],
    syms:   &'a [u8],
}

impl<'a> CodebookDescReader<u8> for HuffDescReader<'a> {
    fn bits(&mut self, idx: usize) -> u8 { self.bits[idx] }
    fn code(&mut self, idx: usize) -> u32 { u32::from(self.codes[idx]) }
    fn sym (&mut self, idx: usize) -> u8 { self.syms[idx] }
    fn len(&mut self) -> usize { self.syms.len() }
}

/// JPEG codebooks for reading block coefficients.
#[derive(Default)]
pub struct JCodebooks {
    pub codebook:   [[Option<Codebook<u8>>; 4]; 2],
}

impl JCodebooks {
    /// Creates a codebook using JPEG-style description consisting of code lengths and symbol values.
    pub fn generate_cb(lens: &[u8; 16], syms: &[u8]) -> DecoderResult<Codebook<u8>> {
        let mut codes = [0; 256];
        let mut bits = [0; 256];

        let mut iter = bits.iter_mut();
        for (i, &len) in lens.iter().enumerate() {
            for _ in 0..len {
                *iter.next().unwrap() = (i + 1) as u8;
            }
        }
        let mut code = 0;
        let mut si = bits[0];
        let mut idx = 0;
        while idx < syms.len() {
            while idx < syms.len() && bits[idx] == si {
                codes[idx] = code;
                code += 1;
                idx  += 1;
            }
            while idx < syms.len() && bits[idx] != si {
                code <<= 1;
                si += 1;
            }
        }

        let mut cbr = HuffDescReader { codes: &codes, bits: &bits, syms };
        Ok(Codebook::new(&mut cbr, CodebookMode::MSB)?)
    }
    /// Creates a codebook for specific coefficient type and number.
    pub fn build_default_cb(dc: bool, idx: usize) -> DecoderResult<Codebook<u8>> {
        if idx > 1 {
            return Err(DecoderError::InvalidData);
        }
        if dc {
            Self::generate_cb(&DC_LENS[idx], &DC_SYMS)
        } else {
            Self::generate_cb(&AC_LENS[idx], AC_SYMS[idx])
        }
    }
    /// Creates a `JCodebooks` instance with the default JPEG codebook.
    pub fn default_jpeg_codebooks() -> Self {
        let dc0_cb = Self::build_default_cb(true, 0).unwrap();
        let dc1_cb = Self::build_default_cb(true, 1).unwrap();
        let ac0_cb = Self::build_default_cb(false, 0).unwrap();
        let ac1_cb = Self::build_default_cb(false, 1).unwrap();
        Self {
            codebook: [
                [Some(dc0_cb), Some(dc1_cb), None, None],
                [Some(ac0_cb), Some(ac1_cb), None, None]
            ]
        }
    }
}

fn read_dc(br: &mut BitReader, cb: &Codebook<u8>) -> DecoderResult<i16> {
    let cat                             = br.read_cb(cb)?;
    match cat {
        0 => Ok(0),
        1..=11 => {
            let add_bits                    = br.read(cat)? as i16;
            let pivot = 1 << (cat - 1);
            if add_bits < pivot {
                Ok(add_bits + 1 - pivot * 2)
            } else {
                Ok(add_bits)
            }
        },
        _ => Err(DecoderError::InvalidData)
    }
}

fn read_ac(br: &mut BitReader, cb: &Codebook<u8>) -> DecoderResult<(usize, i16)> {
    let val                             = br.read_cb(cb)?;
    let run = usize::from(val >> 4);
    let cat = val & 0xF;
    let level = if cat != 0 {
            if cat >= 11 {
                return Err(DecoderError::InvalidData);
            }
            let add_bits                = br.read(cat)? as i16;
            let pivot = 1 << (cat - 1);
            if add_bits < pivot {
                add_bits + 1 - pivot * 2
            } else {
                add_bits
            }
        } else if run == 0 || run == 15 {
            0
        } else {
            return Err(DecoderError::InvalidData);
        };
    Ok((run, level))
}

/// Decodes a single 8x8 coefficient block.
pub fn read_block(br: &mut BitReader, blk: &mut [i16; 64], dc_cb: &Codebook<u8>, ac_cb: &Codebook<u8>, ss: usize, se: usize, qmat: &[i16; 64]) -> DecoderResult<()> {
    if ss == 0 {
        blk[0] = read_dc(br, dc_cb)?;
        blk[0] *= qmat[0];
    }
    let mut idx = 1;
    while idx <= se {
        let (run, level) = read_ac(br, ac_cb)?;
        if run == 0 && level == 0 {
            break;
        }
        idx += run;
        if idx >= 64 {
            return Err(DecoderError::InvalidData);
        }
        blk[ZIGZAG[idx]] = level * qmat[idx];
        idx += 1;
    }
    Ok(())
}

/// Base JPEG decoder.
pub struct JPEGDecoder {
    /// Quantisation matrices.
    pub quant:      [[i16; 64]; 4],
    /// Quantisation matrix selectors for each component.
    pub qselect:    [u8; MAX_CHROMATONS],
    /// Component IDs.
    pub comp_id:    [u8; MAX_CHROMATONS],
    /// Component subsampling modes.
    pub subsamp:    [u8; MAX_CHROMATONS],
    /// Coefficient codebooks
    pub cb:         JCodebooks,
    /// Image width.
    pub width:      usize,
    /// Image height.
    pub height:     usize,
    /// Pixel depth.
    pub depth:      u8,
    /// Maximum horisontal subsampling.
    pub max_h:      u8,
    /// Maximum vertical subsampling.
    pub max_v:      u8,
    /// Codec-specific function for DC predicting in the decoded blocks prior to IDCT.
    pub blk_pred:   fn (usize, &mut [i16; 64], &mut [i16; MAX_CHROMATONS]),
}

/// Default block DC prediction function.
pub fn default_blk_pred(comp_no: usize, blk: &mut [i16; 64], last_dc: &mut [i16; MAX_CHROMATONS]) {
    blk[0] += last_dc[comp_no];
    last_dc[comp_no] = blk[0];
}

impl Default for JPEGDecoder {
    fn default() -> Self {
        Self {
            quant:      [[0; 64]; 4],
            qselect:    [0; MAX_CHROMATONS],
            subsamp:    [0; MAX_CHROMATONS],
            comp_id:    [0; MAX_CHROMATONS],
            cb:         JCodebooks::default(),
            width:      0,
            height:     0,
            depth:      0,
            max_h:      0,
            max_v:      0,
            blk_pred:   default_blk_pred,
        }
    }
}

impl JPEGDecoder {
    /// Creates a new instance of JPEG decoder.
    pub fn new() -> Self { Self::default() }
    /// Decodes a whole JPEG picture assuming interleaved components.
    pub fn decode_image(&mut self, jtype: JPEGType, src: &[u8], src2: Option<&[u8]>, buf: NAVideoBufferRef<u8>, ci: &[ComponentInfo]) -> DecoderResult<()> {
        if let Some(field2) = src2 {
            self.decode_scan(jtype, src, buf.clone(), ci, 0, 63, Some(false))?;
            self.decode_scan(jtype, field2, buf, ci, 0, 63, Some(true))?;
            Ok(())
        } else {
            self.decode_scan(jtype, src, buf, ci, 0, 63, None)?;
            Ok(())
        }
    }
    /// Decodes a single scan of JPEG data.
    #[allow(clippy::too_many_arguments)]
    pub fn decode_scan(&mut self, jtype: JPEGType, src: &[u8], mut buf: NAVideoBufferRef<u8>, ci: &[ComponentInfo], ss: usize, se: usize, imode: Option<bool>) -> DecoderResult<usize> {
        if jtype != JPEGType::Baseline {
            return Err(DecoderError::NotImplemented);
        }
        let num_components = ci.len();
        let mut last_dc = [1024; MAX_CHROMATONS];
        let mut dc_cbs = Vec::with_capacity(num_components);
        let mut ac_cbs = Vec::with_capacity(num_components);
        let mut qmats = [&self.quant[0]; MAX_CHROMATONS];
        for (i, cinfo) in ci.iter().enumerate() {
            dc_cbs.push(if let Some(ref cb) = self.cb.codebook[0][cinfo.dc_table_id] {
                    cb
                } else { unreachable!(); });
            ac_cbs.push(if let Some(ref cb) = self.cb.codebook[1][cinfo.ac_table_id] {
                    cb
                } else { unreachable!(); });
            qmats[i] = &self.quant[self.qselect[cinfo.component_id] as usize];
        }

        let frm = NASimpleVideoFrame::from_video_buf(&mut buf).unwrap();

        let mut br = BitReader::new(src, BitReaderMode::BE);

        let mut offs = [0; MAX_CHROMATONS];
        let mut stride = [0; MAX_CHROMATONS];
        for ((doff, stride), ci) in offs.iter_mut().zip(stride.iter_mut()).zip(ci.iter()) {
            *doff = frm.offset[ci.component_id];
            *stride = frm.stride[ci.component_id];
        }
        let mut nblks = [0; MAX_CHROMATONS];
        let mut ystep = [0; MAX_CHROMATONS];
        let mut hstep = 8;
        let mut vstep = 8;
        if num_components > 1 {
            for (i, cinfo) in ci.iter().enumerate() {
                let hs = (self.subsamp[cinfo.component_id] >> 4)  as usize;
                let vs = (self.subsamp[cinfo.component_id] & 0xF) as usize;
                hstep = hstep.max(hs * 8);
                vstep = vstep.max(vs * 8);
                nblks[i] = hs * vs;
                ystep[i] = vs * 8;
            }
        } else {
            let subsamp = self.subsamp[ci[0].component_id];
            nblks[0] = 1;
            hstep = usize::from(8 * self.max_h / (subsamp >> 4));
            vstep = usize::from(8 * self.max_h / (subsamp & 0xF));
            ystep[0] = 8;
        }

        let mut blocks;
        let height = if imode.is_none() { self.height } else { self.height / 2 };
        let mut stride = frm.stride;
        if imode.is_some() {
            for (strd, off) in stride.iter_mut().zip(offs.iter_mut()) {
                if imode == Some(true) {
                    *off += *strd;
                }
                *strd <<= 1;
            }
        }
        for _y in (0..height).step_by(vstep) {
            for x in 0..(self.width + hstep - 1) / hstep {
                for i in 0..num_components {
                    blocks = [[0; 64]; 4];
                    for blk in blocks[..nblks[i]].iter_mut() {
                        read_block(&mut br, blk, dc_cbs[i], ac_cbs[i], ss, se, qmats[i])?;
                        (self.blk_pred)(i, blk, &mut last_dc);
                        idct(blk);
                    }
                    match self.subsamp[i] {
                        0x11 | 0x44 => {
                            put_block(&blocks[0], &mut frm.data[offs[i] + x * 8..], stride[i]);
                        },
                        0x21 => {
                            put_block(&blocks[0], &mut frm.data[offs[i] + x * 16..], stride[i]);
                            put_block(&blocks[1], &mut frm.data[offs[i] + x * 16 + 8..], stride[i]);
                        },
                        0x12 => {
                            put_block(&blocks[0], &mut frm.data[offs[i] + x * 8..], stride[i]);
                            put_block(&blocks[1], &mut frm.data[offs[i] + x * 8 + stride[i] * 8..], stride[i]);
                        },
                        0x22 => {
                            #[allow(clippy::needless_range_loop)]
                            for j in 0..4 {
                                put_block(&blocks[j], &mut frm.data[offs[i] + x * 16 + (j & 1) * 8 + (j >> 1) * 8 * stride[i]..], stride[i]);
                            }
                        },
                        _ => unreachable!(),
                    };
                }
            }
            for i in 0..num_components {
                offs[i] += stride[i] * ystep[i];
            }
        }

        Ok((br.tell() + 7) / 8)
    }
}

/// Default JPEG luma quantisation matrix.
pub static DEF_LUMA_QUANT: [u8; 64] = [
     16,  11,  12,  14,  12,  10,  16,  14,
     13,  14,  18,  17,  16,  19,  24,  40,
     26,  24,  22,  22,  24,  49,  35,  37,
     29,  40,  58,  51,  61,  60,  57,  51,
     56,  55,  64,  72,  92,  78,  64,  68,
     87,  69,  55,  56,  80, 109,  81,  87,
     95,  98, 103, 104, 103,  62,  77, 113,
    121, 112, 100, 120,  92, 101, 103,  99
];
/// Default JPEG luma quantisation matrix.
pub static DEF_CHROMA_QUANT: [u8; 64] = [
    17, 18, 18, 24, 21, 24, 47, 26,
    26, 47, 99, 66, 56, 66, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99
];


pub static DC_LENS: [[u8; 16]; 2] = [
    [ 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0 ],
    [ 0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0 ]
];
pub static DC_SYMS: [u8; 12] = [ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 ];
pub static AC_LENS: [[u8; 16]; 2] = [
    [ 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 125 ],
    [ 0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 119 ]
];
pub static AC_SYMS: [&[u8]; 2] = [
  &[
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12,
    0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
    0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08,
    0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0,
    0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16,
    0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
    0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
    0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
    0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
    0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79,
    0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98,
    0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
    0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6,
    0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5,
    0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4,
    0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea,
    0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa
  ],
  &[
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21,
    0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
    0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91,
    0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0,
    0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34,
    0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
    0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38,
    0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
    0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
    0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
    0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78,
    0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96,
    0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5,
    0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4,
    0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
    0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2,
    0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
    0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9,
    0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa
  ]
];
