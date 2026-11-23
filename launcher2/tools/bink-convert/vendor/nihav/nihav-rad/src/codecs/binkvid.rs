use nihav_core::codecs::*;
use nihav_core::io::byteio::*;
use nihav_core::io::bitreader::*;
use nihav_core::io::codebook::*;
use nihav_codec_support::codecs::{IPShuffler, HAMShuffler};

use super::binkviddata::*;

const SKIP_BLOCK: u8 = 0;
const SCALED_BLOCK: u8 = 1;
const MOTION_BLOCK: u8 = 2;
const RUN_BLOCK: u8 = 3;
const RESIDUE_BLOCK: u8 = 4;
const INTRA_BLOCK: u8 = 5;
const FILL_BLOCK: u8 = 6;
const INTER_BLOCK: u8 = 7;
const PATTERN_BLOCK: u8 = 8;
const RAW_BLOCK: u8 = 9;

#[derive(Default, Clone,Copy)]
struct Tree {
    id:     usize,
    syms:   [u8; 16],
}

impl Tree {
    #[allow(clippy::collapsible_else_if)]
    fn read_desc(&mut self, br: &mut BitReader) -> DecoderResult<()> {
        self.id                                 = br.read(4)? as usize;
        if self.id == 0 {
            for i in 0..16 { self.syms[i] = i as u8; }
        } else {
            if br.read_bool()? {
                let len                         = br.read(3)? as usize;
                let mut present: [bool; 16] = [false; 16];
                for i in 0..=len {
                    self.syms[i]                = br.read(4)? as u8;
                    present[self.syms[i] as usize] = true;
                }
                let mut idx = len + 1;
                for (i, &present) in present.iter().enumerate() {
                    if present { continue; }
                    self.syms[idx] = i as u8;
                    idx += 1;
                }
            } else {
                let len                         = br.read(2)? as usize;
                let mut syms: [u8; 16] = [ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15];
                let mut tmp: [u8; 16] = [0; 16];
                for bits in 0..=len {
                    let size = 1 << bits;
                    for arr in syms.chunks_mut(size * 2) {
                        let mut ptr0 = 0;
                        let mut ptr1 = size;
                        let mut optr = 0;
                        while (ptr0 < size) && (ptr1 < size * 2) {
                            if !br.read_bool()? {
                                tmp[optr] = arr[ptr0];
                                ptr0 += 1;
                            } else {
                                tmp[optr] = arr[ptr1];
                                ptr1 += 1;
                            }
                            optr += 1;
                        }
                        while ptr0 < size {
                            tmp[optr] = arr[ptr0];
                            ptr0 += 1;
                            optr += 1;
                        }
                        while ptr1 < size * 2 {
                            tmp[optr] = arr[ptr1];
                            ptr1 += 1;
                            optr += 1;
                        }
                        arr.copy_from_slice(&tmp[0..size * 2]);
                    }
                }
                self.syms = syms;
            }
        }
        Ok(())
    }
    fn read_sym(&self, br: &mut BitReader, trees: &BinkTrees) -> DecoderResult<u8> {
        let idx                                 = br.read_cb(&trees.cb[self.id])?;
        Ok(self.syms[idx as usize])
    }
}

#[derive(Default)]
struct Bundle<T: Copy> {
    tree:       Tree,
    data:       Vec<T>,
    dec_pos:    usize,
    read_pos:   usize,
    bits:       u8,
}

impl<T:Copy> Bundle<T> {
    fn binkb_reset(&mut self, bits: u8) {
        self.bits       = bits;
        self.dec_pos    = 0;
        self.read_pos   = 0;
    }
    fn reset(&mut self) {
        self.dec_pos = 0;
        self.read_pos = 0;
    }
    fn read_desc(&mut self, br: &mut BitReader) -> DecoderResult<()> {
        self.dec_pos = 0;
        self.read_pos = 0;
        self.tree.read_desc(br)?;
        Ok(())
    }
    fn read_len(&mut self, br: &mut BitReader) -> DecoderResult<usize> {
        if self.read_pos < self.dec_pos { return Ok(0); }
        let len                                 = br.read(self.bits)? as usize;
        if len == 0 {
            self.dec_pos = self.data.len();
            self.read_pos = self.data.len() - 1;
        }
        Ok(len)
    }
    fn read_len_binkb(&mut self, br: &mut BitReader) -> DecoderResult<usize> {
        if self.read_pos < self.dec_pos { return Ok(0); }
        let len                                 = br.read(13)? as usize;
        if len == 0 {
            self.dec_pos = self.data.len();
            self.read_pos = self.data.len() - 1;
        }
        Ok(len)
    }
    fn get_val(&mut self) -> DecoderResult<T> {
        validate!(self.read_pos < self.dec_pos);
        let val = self.data[self.read_pos];
        self.read_pos += 1;
        Ok(val)
    }
}

const BLOCK_TYPE_RUNS: [usize; 4] = [ 4, 8, 12, 32 ];
impl Bundle<u8> {
    fn read_binkb(&mut self, br: &mut BitReader) -> DecoderResult<()> {
        let len = self.read_len_binkb(br)?;
        if len == 0 { return Ok(()); }
        let end = self.dec_pos + len;
        validate!(end <= self.data.len());
        for i in 0..len {
            self.data[self.dec_pos + i]         = br.read(self.bits)? as u8;
        }
        self.dec_pos += len;
        Ok(())
    }
    fn read_runs(&mut self, br: &mut BitReader, trees: &BinkTrees) -> DecoderResult<()> {
        let len = self.read_len(br)?;
        if len == 0 { return Ok(()); }
        let end = self.dec_pos + len;
        validate!(end <= self.data.len());
        if br.read_bool()? {
            let val                             = br.read(4)? as u8;
            for i in 0..len { self.data[self.dec_pos + i] = val; }
            self.dec_pos += len;
        } else {
            while self.dec_pos < end {
                self.data[self.dec_pos] = self.tree.read_sym(br, trees)?;
                self.dec_pos += 1;
            }
        }
        Ok(())
    }
    fn read_block_types(&mut self, br: &mut BitReader, trees: &BinkTrees) -> DecoderResult<()> {
        let len = self.read_len(br)?;
        if len == 0 { return Ok(()); }
        let end = self.dec_pos + len;
        validate!(end <= self.data.len());
        if br.read_bool()? {
            let val                             = br.read(4)? as u8;
            for i in 0..len { self.data[self.dec_pos + i] = val; }
            self.dec_pos += len;
        } else {
            let mut last = 0;
            while self.dec_pos < end {
                let val = self.tree.read_sym(br, trees)?;
                if val < 12 {
                    self.data[self.dec_pos] = val;
                    self.dec_pos += 1;
                    last = val;
                } else {
                    let run = BLOCK_TYPE_RUNS[(val - 12) as usize];
                    validate!(self.dec_pos + run <= end);
                    for i in 0..run {
                        self.data[self.dec_pos + i] = last;
                    }
                    self.dec_pos += run;
                }
            }
        }
        Ok(())
    }
    fn read_patterns(&mut self, br: &mut BitReader, trees: &BinkTrees) -> DecoderResult<()> {
        let len = self.read_len(br)?;
        if len == 0 { return Ok(()); }
        let end = self.dec_pos + len;
        validate!(end <= self.data.len());
        for i in 0..len {
            let pat_lo = self.tree.read_sym(br, trees)?;
            let pat_hi = self.tree.read_sym(br, trees)?;
            self.data[self.dec_pos + i] = pat_lo | (pat_hi << 4);
        }
        self.dec_pos += len;
        Ok(())
    }
    fn cvt_color(lo: u8, hi: u8, new_bink: bool) -> u8 {
        let val = lo | (hi << 4);
        if !new_bink {
            let sign = ((val as i8) >> 7) as u8;
            ((val & 0x7F) ^ sign).wrapping_sub(sign) ^ 0x80
        } else {
            val
        }
    }
    fn read_colors(&mut self, br: &mut BitReader, trees: &BinkTrees, col_hi: &[Tree; 16], col_last: &mut u8, new_bink: bool) -> DecoderResult<()> {
        let len = self.read_len(br)?;
        if len == 0 { return Ok(()); }
        let end = self.dec_pos + len;
        validate!(end <= self.data.len());
        let mut last = *col_last;
        if br.read_bool()? {
            last = col_hi[last as usize].read_sym(br, trees)?;
            let lo = self.tree.read_sym(br, trees)?;
            let val = Self::cvt_color(lo, last, new_bink);
            for i in 0..len { self.data[self.dec_pos + i] = val; }
            self.dec_pos += len;
        } else {
            while self.dec_pos < end {
                last = col_hi[last as usize].read_sym(br, trees)?;
                let lo = self.tree.read_sym(br, trees)?;
                let val = Self::cvt_color(lo, last, new_bink);
                self.data[self.dec_pos] = val;
                self.dec_pos += 1;
            }
        }
        *col_last = last;
        Ok(())
    }
}

impl Bundle<i8> {
    fn read_binkb(&mut self, br: &mut BitReader) -> DecoderResult<()> {
        let len = self.read_len_binkb(br)?;
        if len == 0 { return Ok(()); }
        let end = self.dec_pos + len;
        validate!(end <= self.data.len());
        let bias = 1 << (self.bits - 1);
        for i in 0..len {
            self.data[self.dec_pos + i]         = (br.read(self.bits)? as i8) - bias;
        }
        self.dec_pos += len;
        Ok(())
    }
    fn read_motion_values(&mut self, br: &mut BitReader, trees: &BinkTrees) -> DecoderResult<()> {
        let len = self.read_len(br)?;
        if len == 0 { return Ok(()); }
        let end = self.dec_pos + len;
        validate!(end <= self.data.len());
        if br.read_bool()? {
            let mut val                         = br.read(4)? as i8;
            if val != 0 && br.read_bool()? { val = -val; }
            for i in 0..len { self.data[self.dec_pos + i] = val; }
            self.dec_pos += len;
        } else {
            while self.dec_pos < end {
                self.data[self.dec_pos] = self.tree.read_sym(br, trees)? as i8;
                if self.data[self.dec_pos] != 0 && br.read_bool()? {
                    self.data[self.dec_pos] = -self.data[self.dec_pos];
                }
                self.dec_pos += 1;
            }
        }
        Ok(())
    }
}

const DC_START_BITS: u8 = 11;
impl Bundle<u16> {
    fn read_binkb(&mut self, br: &mut BitReader) -> DecoderResult<()> {
        let len = self.read_len_binkb(br)?;
        if len == 0 { return Ok(()); }
        let end = self.dec_pos + len;
        validate!(end <= self.data.len());
        for i in 0..len {
            self.data[self.dec_pos + i]         = br.read(self.bits)? as u16;
        }
        self.dec_pos += len;
        Ok(())
    }
    fn read_dcs(&mut self, br: &mut BitReader, start_bits: u8) -> DecoderResult<()> {
        let len = self.read_len(br)?;
        if len == 0 { return Ok(()); }
        let end = self.dec_pos + len;
        validate!(end <= self.data.len());
        let mut val                             = br.read(start_bits)? as u16;
        self.data[self.dec_pos] = val;
        self.dec_pos += 1;
        for i in (1..len).step_by(8) {
            let seg_len = (len - i).min(8);
            let bits                            = br.read(4)? as u8;
            if bits != 0 {
                for _ in 0..seg_len {
                    let diff                    = br.read(bits)? as u16;
                    let res = if diff != 0 && br.read_bool()? {
                            val.checked_sub(diff)
                        } else {
                            val.checked_add(diff)
                        };
                    validate!(res.is_some());
                    val = res.unwrap();
                    self.data[self.dec_pos] = val;
                    self.dec_pos += 1;
                }
            } else {
                for _ in 0..seg_len {
                    self.data[self.dec_pos] = val;
                    self.dec_pos += 1;
                }
            }
        }
        Ok(())
    }
}

impl Bundle<i16> {
    fn read_binkb(&mut self, br: &mut BitReader) -> DecoderResult<()> {
        let len = self.read_len_binkb(br)?;
        if len == 0 { return Ok(()); }
        let end = self.dec_pos + len;
        validate!(end <= self.data.len());
        let bias = 1 << (self.bits - 1);
        for i in 0..len {
            self.data[self.dec_pos + i]         = (br.read(self.bits)? as i16) - bias;
        }
        self.dec_pos += len;
        Ok(())
    }
    fn read_dcs(&mut self, br: &mut BitReader, start_bits: u8) -> DecoderResult<()> {
        let len = self.read_len(br)?;
        if len == 0 { return Ok(()); }
        let end = self.dec_pos + len;
        validate!(end <= self.data.len());
        let mut val                             = br.read(start_bits - 1)? as i16;
        if val != 0 && br.read_bool()? {
            val = -val;
        }
        self.data[self.dec_pos] = val;
        self.dec_pos += 1;
        for i in (1..len).step_by(8) {
            let seg_len = (len - i).min(8);
            let bits                            = br.read(4)? as u8;
            if bits != 0 {
                for _ in 0..seg_len {
                    let mut diff                = br.read(bits)? as i16;
                    if diff != 0 && br.read_bool()? {
                        diff = -diff;
                    }
                    let res = val.checked_add(diff);
                    validate!(res.is_some());
                    val = res.unwrap();
                    self.data[self.dec_pos] = val;
                    self.dec_pos += 1;
                }
            } else {
                for _ in 0..seg_len {
                    self.data[self.dec_pos] = val;
                    self.dec_pos += 1;
                }
            }
        }
        Ok(())
    }
}

struct BinkTrees {
    cb:         [Codebook<u8>; 16],
}

impl Default for BinkTrees {
    fn default() -> Self {
        let cb = unsafe {
                let mut ucb: std::mem::MaybeUninit::<[Codebook<u8>; 16]> = std::mem::MaybeUninit::uninit();
                for i in 0..16 {
                    let mut cr = TableCodebookDescReader::new(&BINK_TREE_CODES[i], &BINK_TREE_BITS[i], |idx| idx as u8);
                    std::ptr::write(&mut (*ucb.as_mut_ptr())[i], Codebook::new(&mut cr, CodebookMode::LSB).unwrap());
                }
                ucb.assume_init()
            };
        Self { cb }
    }
}

const A1: i32 =  2896;
const A2: i32 =  2217;
const A3: i32 =  3784;
const A4: i32 = -5352;

macro_rules! idct {
    ($src: expr, $sstep: expr, $dst: expr, $dstep: expr, $off: expr, $bias: expr, $shift: expr) => {
        let a0 = $src[$off + 0 * $sstep] + $src[$off + 4 * $sstep];
        let a1 = $src[$off + 0 * $sstep] - $src[$off + 4 * $sstep];
        let a2 = $src[$off + 2 * $sstep] + $src[$off + 6 * $sstep];
        let a3 = A1.wrapping_mul($src[$off + 2 * $sstep] - $src[$off + 6 * $sstep]) >> 11;
        let a4 = $src[$off + 5 * $sstep] + $src[$off + 3 * $sstep];
        let a5 = $src[$off + 5 * $sstep] - $src[$off + 3 * $sstep];
        let a6 = $src[$off + 1 * $sstep] + $src[$off + 7 * $sstep];
        let a7 = $src[$off + 1 * $sstep] - $src[$off + 7 * $sstep];
        let b0 = a4 + a6;
        let b1 = A3.wrapping_mul(a5 + a7) >> 11;
        let b2 = (A4.wrapping_mul(a5) >> 11) - b0 + b1;
        let b3 = (A1.wrapping_mul(a6 - a4) >> 11) - b2;
        let b4 = (A2.wrapping_mul(a7) >> 11) + b3 - b1;
        let c0 = a0 + a2;
        let c1 = a0 - a2;
        let c2 = a1 + (a3 - a2);
        let c3 = a1 - (a3 - a2);

        $dst[$off + 0 * $dstep] = (c0 + b0 + $bias) >> $shift;
        $dst[$off + 1 * $dstep] = (c2 + b2 + $bias) >> $shift;
        $dst[$off + 2 * $dstep] = (c3 + b3 + $bias) >> $shift;
        $dst[$off + 3 * $dstep] = (c1 - b4 + $bias) >> $shift;
        $dst[$off + 4 * $dstep] = (c1 + b4 + $bias) >> $shift;
        $dst[$off + 5 * $dstep] = (c3 - b3 + $bias) >> $shift;
        $dst[$off + 6 * $dstep] = (c2 - b2 + $bias) >> $shift;
        $dst[$off + 7 * $dstep] = (c0 - b0 + $bias) >> $shift;
    };
}

#[derive(Default)]
struct BinkDecoder {
    info:       NACodecInfoRef,
    ips:        IPShuffler,
    hams:       HAMShuffler<u8>,

    is_ver_b:   bool,
    is_ver_i:   bool,
    has_alpha:  bool,
    is_gray:    bool,
    swap_uv:    bool,
    key_frame:  bool,

    cur_w:      usize,
    cur_h:      usize,
    cur_plane:  usize,

    colhi_tree: [Tree; 16],
    col_last:   u8,

    btype:      Bundle<u8>,
    sbtype:     Bundle<u8>,
    colors:     Bundle<u8>,
    pattern:    Bundle<u8>,
    xoff:       Bundle<i8>,
    yoff:       Bundle<i8>,
    intradc:    Bundle<u16>,
    interdc:    Bundle<i16>,
    intraq:     Bundle<u8>,
    interq:     Bundle<u8>,
    nresidues:  Bundle<u8>,
    run:        Bundle<u8>,

    trees:      BinkTrees,

    qmat_b:     QuantMats,

    strict_mode: bool,
}

fn calc_len(size: usize) -> u8 {
    (32 - ((size + 511) as u32).leading_zeros()) as u8
}

#[allow(clippy::too_many_arguments)]
impl BinkDecoder {
    fn new() -> Self {
        Self::default()
    }
    fn init_bundle_bufs(&mut self, bw: usize, bh: usize) {
        let size = bw * bh * 64;
        self.btype.data.resize(size, 0);
        self.sbtype.data.resize(size, 0);
        self.colors.data.resize(size, 0);
        self.pattern.data.resize(size, 0);
        self.xoff.data.resize(size, 0);
        self.yoff.data.resize(size, 0);
        self.intradc.data.resize(size, 0);
        self.interdc.data.resize(size, 0);
        self.intraq.data.resize(size, 0);
        self.interq.data.resize(size, 0);
        self.nresidues.data.resize(size, 0);
        self.run.data.resize(size, 0);
    }
    fn init_bundle_lengths(&mut self, w: usize, bw: usize) {
        let w = (w + 7) & !7;
        self.btype.bits     = calc_len(w >> 3);
        self.sbtype.bits    = calc_len(w >> 4);
        self.colors.bits    = calc_len(bw * 64);
        self.pattern.bits   = calc_len(bw * 8);
        self.xoff.bits      = calc_len(w >> 3);
        self.yoff.bits      = calc_len(w >> 3);
        self.intradc.bits   = calc_len(w >> 3);
        self.interdc.bits   = calc_len(w >> 3);
        self.run.bits       = calc_len(bw * 48);
    }
    fn init_bundle_lengths_binkb(&mut self) {
        self.btype.binkb_reset(4);
        self.colors.binkb_reset(8);
        self.pattern.binkb_reset(8);
        self.xoff.binkb_reset(5);
        self.yoff.binkb_reset(5);
        self.intradc.binkb_reset(11);
        self.interdc.binkb_reset(11);
        self.intraq.binkb_reset(4);
        self.interq.binkb_reset(4);
        self.nresidues.binkb_reset(7);
    }
    fn read_bundles_desc(&mut self, br: &mut BitReader) -> DecoderResult<()> {
        self.btype.read_desc(br)?;
        self.sbtype.read_desc(br)?;
        for el in &mut self.colhi_tree {
            el.read_desc(br)?;
        }
        self.col_last = 0;
        self.colors.read_desc(br)?;
        self.pattern.read_desc(br)?;
        self.xoff.read_desc(br)?;
        self.yoff.read_desc(br)?;
        self.intradc.reset();
        self.interdc.reset();
        self.run.read_desc(br)?;
        Ok(())
    }
    fn read_bundles_binkb(&mut self, br: &mut BitReader) -> DecoderResult<()> {
        self.btype.read_binkb(br)?;
        self.colors.read_binkb(br)?;
        self.pattern.read_binkb(br)?;
        self.xoff.read_binkb(br)?;
        self.yoff.read_binkb(br)?;
        self.intradc.read_binkb(br)?;
        self.interdc.read_binkb(br)?;
        self.intraq.read_binkb(br)?;
        self.interq.read_binkb(br)?;
        self.nresidues.read_binkb(br)?;
        Ok(())
    }
    fn read_bundles(&mut self, br: &mut BitReader) -> DecoderResult<()> {
        self.btype.read_block_types(br, &self.trees)?;
        self.sbtype.read_block_types(br, &self.trees)?;
        self.colors.read_colors(br, &self.trees, &self.colhi_tree, &mut self.col_last, self.is_ver_i)?;
        self.pattern.read_patterns(br, &self.trees)?;
        self.xoff.read_motion_values(br, &self.trees)?;
        self.yoff.read_motion_values(br, &self.trees)?;
        self.intradc.read_dcs(br, DC_START_BITS)?;
        self.interdc.read_dcs(br, DC_START_BITS)?;
        self.run.read_runs(br, &self.trees)?;
        Ok(())
    }

    fn put_block(&self, block: &[u8; 64], dst: &mut [u8], off: usize, stride: usize, scaled: bool) {
        if !scaled {
            for (src, dline) in block.chunks_exact(8).zip(dst[off..].chunks_mut(stride)) {
                dline[..8].copy_from_slice(src);
            }
        } else {
            for (src, dlines) in block.chunks_exact(8).zip(dst[off..].chunks_mut(stride * 2)) {
                let (dline0, dline1) = dlines.split_at_mut(stride);
                for (pair, &pix) in dline0.chunks_exact_mut(2).zip(src[..8].iter()) {
                    pair[0] = pix;
                    pair[1] = pix;
                }
                for (pair, &pix) in dline1.chunks_exact_mut(2).zip(src[..8].iter()) {
                    pair[0] = pix;
                    pair[1] = pix;
                }
            }
        }
    }
    fn copy_block(&mut self, dst: &mut [u8], mut off: usize, stride: usize, bx: usize, by: usize, xoff: i8, yoff: i8) -> DecoderResult<()> {
        if let Some(prev_buf) = self.ips.get_ref() {
            let xoff = ((bx * 8) as isize) + (xoff as isize);
            let yoff = ((by * 8) as isize) + (yoff as isize);
            validate!((xoff >= 0) && (xoff + 8 <= (self.cur_w as isize)));
            validate!((yoff >= 0) && (yoff + 8 <= (self.cur_h as isize)));
            let pstride = prev_buf.get_stride(self.cur_plane);
            let mut poff = prev_buf.get_offset(self.cur_plane) + (xoff as usize) + (yoff as usize) * pstride;
            let pdata = prev_buf.get_data();
            let ppix = pdata.as_slice();
            for _ in 0..8 {
                let src = &ppix[poff..][..8];
                let out = &mut dst[off..][..8];
                out.copy_from_slice(src);
                off += stride;
                poff += pstride;
            }
            Ok(())
        } else if !self.strict_mode {
            for line in dst[off..].chunks_mut(stride).take(8) {
                for pix in line[..8].iter_mut() {
                    *pix = 0x80;
                }
            }
            Ok(())
        } else {
            Err(DecoderError::MissingReference)
        }
    }
    fn copy_overlapped(&mut self, dst: &mut [u8], mut off: usize, stride: usize, bx: usize, by: usize, xoff: i8, yoff1: i8) -> DecoderResult<()> {
        let ybias = if self.key_frame { -15 } else { 0 };
        let yoff = yoff1 + ybias;

        let xpos = ((bx * 8) as isize) + (xoff as isize);
        let ypos = ((by * 8) as isize) + (yoff as isize);
        validate!((xpos >= 0) && (xpos + 8 <= (self.cur_w as isize)));
        validate!((ypos >= 0) && (ypos + 8 <= (self.cur_h as isize)));

        let mut block: [u8; 64] = [0; 64];
        let mut ref_off = ((off as isize) + (xoff as isize) + (yoff as isize) * (stride as isize)) as usize;
        for row in block.chunks_exact_mut(8) {
            row.copy_from_slice(&dst[ref_off..][..8]);
            ref_off += stride;
        }
        for row in block.chunks_exact(8) {
            let out = &mut dst[off..][..8];
            out.copy_from_slice(row);
            off += stride;
        }

        Ok(())
    }
    fn add_block(&self, coeffs: &[i32; 64], dst: &mut [u8], mut off: usize, stride: usize) {
        for src in coeffs.chunks_exact(8) {
            for i in 0..8 {
                let v = (dst[off + i] as i32) + src[i];
                dst[off + i] = v as u8;
            }
            off += stride;
        }
    }
    fn idct_put(&self, coeffs: &[i32; 64], dst: &mut [u8], mut off: usize, stride: usize) {
        let mut tmp: [i32; 64] = [0; 64];
        let mut row: [i32; 8] = [0; 8];
        for i in 0..8 {
            idct!(coeffs, 8, tmp, 8, i, 0, 0);
        }
        for srow in tmp.chunks_exact(8) {
            idct!(srow, 1, row, 1, 0, 0x7F, 8);
            for i in 0..8 {
                dst[off + i] = row[i] as u8;
            }
            off += stride;
        }
    }
    fn idct_add(&self, coeffs: &[i32; 64], dst: &mut [u8], mut off: usize, stride: usize) {
        let mut tmp: [i32; 64] = [0; 64];
        let mut row: [i32; 8] = [0; 8];
        for i in 0..8 {
            idct!(coeffs, 8, tmp, 8, i, 0, 0);
        }
        for srow in tmp.chunks_exact(8) {
            idct!(srow, 1, row, 1, 0, 0x7F, 8);
            for i in 0..8 {
                let v = (dst[off + i] as i32) + row[i];
                dst[off + i] = v as u8;
            }
            off += stride;
        }
    }

    fn decode_plane_binkb(&mut self, br: &mut BitReader, plane_no: usize, buf: &mut NAVideoBuffer<u8>) -> DecoderResult<()> {
        let stride = buf.get_stride(plane_no);
        let mut off = buf.get_offset(plane_no);
        let (width, height) = buf.get_dimensions(plane_no);
        let data = buf.get_data_mut().unwrap();
        let dst = data.as_mut_slice();
        let bw = (width  + 7) >> 3;
        let bh = (height + 7) >> 3;
        self.cur_w = (width + 7) & !7;
        self.cur_h = (height + 7) & !7;
        self.cur_plane = plane_no;
        self.init_bundle_lengths_binkb();
        for by in 0..bh {
            self.read_bundles_binkb(br)?;
            for bx in 0..bw {
                let mut coeffs: [i32; 64] = [0; 64];
                let btype = self.btype.get_val()?;
                match btype {
                    0 => { // skip
                        },
                    1 => { // run
                            let scan = BINK_PATTERNS[br.read(4)? as usize];
                            let mut idx = 0;
                            while idx < 63 {
                                let run         = br.read_bool()?;
                                let len         = (br.read(BINKB_RUN_BITS[idx])? as usize) + 1;
                                validate!(idx + len <= 64);
                                if run {
                                    let val = self.colors.get_val()?;
                                    for j in 0..len {
                                        let pos = scan[idx + j] as usize;
                                        dst[off + (pos >> 3) * stride + (pos & 7)] = val;
                                    }
                                    idx += len;
                                } else {
                                    for _ in 0..len {
                                        let pos = scan[idx] as usize;
                                        dst[off + (pos >> 3) * stride + (pos & 7)] = self.colors.get_val()?;
                                        idx += 1;
                                    }
                                }
                            }
                            if idx == 63 {
                                let pos = scan[idx] as usize;
                                dst[off + (pos >> 3) * stride + (pos & 7)] = self.colors.get_val()?;
                            }
                        },
                    2 => { // intra
                            coeffs[0] = self.intradc.get_val()? as i32;
                            let q = self.intraq.get_val()? as usize;
                            read_dct_coefficients(br, &mut coeffs, &BINK_SCAN, &self.qmat_b.intra_qmat, Some(q))?;
                            self.idct_put(&coeffs, dst, off, stride);
                        },
                    3 => { // residue
                            let xoff = self.xoff.get_val()?;
                            let yoff = self.yoff.get_val()?;
                            self.copy_overlapped(dst, off, stride, bx, by, xoff, yoff)?;
                            let nmasks = self.nresidues.get_val()? as usize;
                            read_residue(br, &mut coeffs, nmasks)?;
                            self.add_block(&coeffs, dst, off, stride);
                        },
                    4 => { // inter
                            let xoff = self.xoff.get_val()?;
                            let yoff = self.yoff.get_val()?;
                            self.copy_overlapped(dst, off, stride, bx, by, xoff, yoff)?;
                            coeffs[0] = self.interdc.get_val()? as i32;
                            let q = self.interq.get_val()? as usize;
                            read_dct_coefficients(br, &mut coeffs, &BINK_SCAN, &self.qmat_b.inter_qmat, Some(q))?;
                            self.idct_add(&coeffs, dst, off, stride);
                        },
                    5 => { // fill
                            let fill = self.colors.get_val()?;
                            for i in 0..8 {
                                for j in 0..8 { dst[off + i * stride + j] = fill; }
                            }
                        },
                    6 => { // pattern
                            let clr: [u8; 2] = [ self.colors.get_val()?, self.colors.get_val()? ];
                            for i in 0..8 {
                                let pattern = self.pattern.get_val()? as usize;
                                for j in 0..8 {
                                    dst[off + i * stride + j] = clr[(pattern >> j) & 1];
                                }
                            }
                        },
                    7 => { // motion block
                            let xoff = self.xoff.get_val()?;
                            let yoff = self.yoff.get_val()?;
                            self.copy_overlapped(dst, off, stride, bx, by, xoff, yoff)?;
                        },
                    8 => { // raw
                            for i in 0..8 {
                                for j in 0..8 {
                                    dst[off + i * stride + j] = self.colors.get_val()?;
                                }
                            }
                        },
                    _ => { return Err(DecoderError::InvalidData); },
                };
                off += 8;
            }
            off += stride * 8 - bw * 8;
        }
        if (br.tell() & 0x1F) != 0 {
            let skip = (32 - (br.tell() & 0x1F)) as u32;
            br.skip(skip)?;
        }
        Ok(())
    }
    fn handle_block(&mut self, br: &mut BitReader, bx: usize, by: usize,
                    dst: &mut [u8], off: usize, stride: usize, btype: u8, scaled: bool) -> DecoderResult<()> {
        let mut oblock: [u8; 64] = [0; 64];
        let mut coeffs: [i32; 64] = [0; 64];
        match btype {
            SKIP_BLOCK => {
                    validate!(!scaled);
                    self.copy_block(dst, off, stride, bx, by, 0, 0)?;
                },
            SCALED_BLOCK => {
                    validate!(!scaled);
                    let sbtype = self.sbtype.get_val()?;
                    self.handle_block(br, bx, by, dst, off, stride, sbtype, true)?;
                },
            MOTION_BLOCK => {
                    validate!(!scaled);
                    let xoff = self.xoff.get_val()?;
                    let yoff = self.yoff.get_val()?;
                    self.copy_block(dst, off, stride, bx, by, xoff, yoff)?;
                },
            RUN_BLOCK => {
                    let scan = BINK_PATTERNS[br.read(4)? as usize];
                    let mut idx = 0;
                    while idx < 63 {
                        let run = (self.run.get_val()? as usize) + 1;
                        validate!(idx + run <= 64);
                        if br.read_bool()? {
                            let val = self.colors.get_val()?;
                            for j in 0..run {
                                oblock[scan[idx + j] as usize] = val;
                            }
                            idx += run;
                        } else {
                            for _ in 0..run {
                                oblock[scan[idx] as usize] = self.colors.get_val()?;
                                idx += 1;
                            }
                        }
                    }
                    if idx == 63 { oblock[scan[63] as usize] = self.colors.get_val()?; }
                    self.put_block(&oblock, dst, off, stride, scaled);
                },
            RESIDUE_BLOCK => {
                    validate!(!scaled);
                    let xoff = self.xoff.get_val()?;
                    let yoff = self.yoff.get_val()?;
                    self.copy_block(dst, off, stride, bx, by, xoff, yoff)?;
                    let nmasks                  = br.read(7)? as usize;
                    read_residue(br, &mut coeffs, nmasks)?;
                    self.add_block(&coeffs, dst, off, stride);
                },
            INTRA_BLOCK => {
                    coeffs[0] = self.intradc.get_val()? as i32;
                    read_dct_coefficients(br, &mut coeffs, &BINK_SCAN, BINK_INTRA_QUANT, None)?;
                    if !scaled {
                        self.idct_put(&coeffs, dst, off, stride);
                    } else {
                        self.idct_put(&coeffs, &mut oblock, 0, 8);
                        self.put_block(&oblock, dst, off, stride, scaled);
                    }
                },
            FILL_BLOCK => {
                    let fill = self.colors.get_val()?;
                    oblock = [fill; 64];
                    self.put_block(&oblock, dst, off, stride, scaled);
                },
            INTER_BLOCK => {
                    validate!(!scaled);
                    let xoff = self.xoff.get_val()?;
                    let yoff = self.yoff.get_val()?;
                    self.copy_block(dst, off, stride, bx, by, xoff, yoff)?;
                    coeffs[0] = self.interdc.get_val()? as i32;
                    read_dct_coefficients(br, &mut coeffs, &BINK_SCAN, BINK_INTER_QUANT, None)?;
                    self.idct_add(&coeffs, dst, off, stride);
                },
            PATTERN_BLOCK => {
                    let clr: [u8; 2] = [ self.colors.get_val()?, self.colors.get_val()? ];
                    for i in 0..8 {
                        let pattern = self.pattern.get_val()? as usize;
                        for j in 0..8 {
                            oblock[i * 8 + j] = clr[(pattern >> j) & 1];
                        }
                    }
                    self.put_block(&oblock, dst, off, stride, scaled);
                },
            RAW_BLOCK => {
                    for i in 0..8 {
                        for j in 0..8 {
                            oblock[i * 8 + j] = self.colors.get_val()?;
                        }
                    }
                    self.put_block(&oblock, dst, off, stride, scaled);
                },
            _ => { return Err(DecoderError::InvalidData); },
        };
        Ok(())
    }
    fn decode_plane(&mut self, br: &mut BitReader, plane_no: usize, buf: &mut NAVideoBuffer<u8>) -> DecoderResult<()> {
        let stride = buf.get_stride(plane_no);
        let mut off = buf.get_offset(plane_no);
        let (width, height) = buf.get_dimensions(plane_no);
        let data = buf.get_data_mut().unwrap();
        let dst = data.as_mut_slice();
        let bw = (width  + 7) >> 3;
        let bh = (height + 7) >> 3;
        self.cur_w = (width + 7) & !7;
        self.cur_h = (height + 7) & !7;
        self.cur_plane = plane_no;
        self.init_bundle_lengths(width.max(8), bw);
        self.read_bundles_desc(br)?;
        for by in 0..bh {
            self.read_bundles(br)?;
            let mut bx = 0;
            while bx < bw {
                let btype = self.btype.get_val()?;
                if btype == SCALED_BLOCK && (by & 1) == 1 { // already decoded scaled block, skip
                    bx += 2;
                    continue;
                }
                self.handle_block(br, bx, by, dst, off + bx * 8, stride, btype, false)?;
                if btype == SCALED_BLOCK {
                    bx += 1;
                }
                bx += 1;
            }
            off += stride * 8;
        }
        if (br.tell() & 0x1F) != 0 {
            let skip = (32 - (br.tell() & 0x1F)) as u32;
            br.skip(skip)?;
        }
        Ok(())
    }
}

fn get_coef(br: &mut BitReader, bits1: u8) -> DecoderResult<i32> {
    let t;
    if bits1 == 1 {
        t = if br.read_bool()? { -1 } else { 1 };
    } else {
        let bits = bits1 - 1;
        let val             = (br.read(bits)? as i32) | (1 << bits);
        if br.read_bool()? {
            t = -val;
        } else {
            t = val;
        }
    }
    Ok(t)
}

fn read_dct_coefficients(br: &mut BitReader, block: &mut [i32; 64], scan: &[usize; 64],
                         quant_matrices: &[[i32; 64]; 16], q: Option<usize>) -> DecoderResult<()> {
    let mut coef_list: [i32; 128] = [0; 128];
    let mut mode_list: [u8; 128] = [0; 128];
    let mut list_start = 64;
    let mut list_end   = 64;
    let mut coef_idx: [usize; 64] = [0; 64];
    let mut coef_count = 0;

    coef_list[list_end] =  4;   mode_list[list_end] = 0;    list_end += 1;
    coef_list[list_end] = 24;   mode_list[list_end] = 0;    list_end += 1;
    coef_list[list_end] = 44;   mode_list[list_end] = 0;    list_end += 1;
    coef_list[list_end] =  1;   mode_list[list_end] = 3;    list_end += 1;
    coef_list[list_end] =  2;   mode_list[list_end] = 3;    list_end += 1;
    coef_list[list_end] =  3;   mode_list[list_end] = 3;    list_end += 1;

    let mut bits1                               = br.read(4)? as u8;
    while bits1 >= 1 {
        let mut list_pos = list_start;
        while list_pos < list_end {
            let ccoef = coef_list[list_pos];
            let mode  = mode_list[list_pos];
            if (mode == 0 && ccoef == 0) || !br.read_bool()? {
                list_pos += 1;
                continue;
            }
            match mode {
                0 | 2 => {
                        if mode == 0 {
                            coef_list[list_pos] = ccoef + 4;
                            mode_list[list_pos] = 1;
                        } else {
                            coef_list[list_pos] = 0;
                            mode_list[list_pos] = 0;
                            list_pos += 1;
                        }
                        for i in 0..4 {
                            if br.read_bool()? {
                                list_start -= 1;
                                coef_list[list_start] = ccoef + i;
                                mode_list[list_start] = 3;
                            } else {
                                let idx = (ccoef + i) as usize;
                                block[scan[idx]] = get_coef(br, bits1)?;
                                coef_idx[coef_count] = idx;
                                coef_count += 1;
                            }
                        }
                    },
                1 => {
                        mode_list[list_pos] = 2;
                        for i in 0..3 {
                            coef_list[list_end] = ccoef + i * 4 + 4;
                            mode_list[list_end] = 2;
                            list_end += 1;
                        }
                    },
                3 => {
                        let idx = ccoef as usize;
                        block[scan[idx]] = get_coef(br, bits1)?;
                        coef_idx[coef_count] = idx;
                        coef_count += 1;
                        coef_list[list_pos] = 0;
                        mode_list[list_pos] = 0;
                        list_pos += 1;
                    },
                _ => unreachable!(),
            };
        }
        bits1 -= 1;
    }

    let q_index = if let Some(qidx) = q { qidx } else { br.read(4)? as usize };
    let qmat = &quant_matrices[q_index];
    block[0] = block[0].wrapping_mul(qmat[0]) >> 11;
    for idx in coef_idx.iter().take(coef_count) {
        block[scan[*idx]] = block[scan[*idx]].wrapping_mul(qmat[*idx]) >> 11;
    }

    Ok(())
}

fn read_residue(br: &mut BitReader, block: &mut [i32; 64], mut masks_count: usize) -> DecoderResult<()> {
    let mut coef_list: [i32; 128] = [0; 128];
    let mut mode_list: [u8; 128] = [0; 128];
    let mut list_start = 64;
    let mut list_end   = 64;
    let mut nz_coef_idx: [usize; 64] = [0; 64];
    let mut nz_coef_count = 0;

    coef_list[list_end] =  4;   mode_list[list_end] = 0;    list_end += 1;
    coef_list[list_end] = 24;   mode_list[list_end] = 0;    list_end += 1;
    coef_list[list_end] = 44;   mode_list[list_end] = 0;    list_end += 1;
    coef_list[list_end] =  0;   mode_list[list_end] = 2;    list_end += 1;

    let mut mask                                = 1 << br.read(3)?;
    while mask > 0 {
        for &idx in nz_coef_idx[..nz_coef_count].iter() {
            if !br.read_bool()? { continue; }
            if block[idx] < 0 {
                block[idx] -= mask;
            } else {
                block[idx] += mask;
            }
            if masks_count == 0 {
                return Ok(());
            }
            masks_count -= 1;
        }
        let mut list_pos = list_start;
        while list_pos < list_end {
            let ccoef = coef_list[list_pos];
            let mode  = mode_list[list_pos];
            if (mode == 0 && ccoef == 0) || !br.read_bool()? {
                list_pos += 1;
                continue;
            }
            match mode {
                0 | 2 => {
                        if mode == 0 {
                            coef_list[list_pos] = ccoef + 4;
                            mode_list[list_pos] = 1;
                        } else {
                            coef_list[list_pos] = 0;
                            mode_list[list_pos] = 0;
                            list_pos += 1;
                        }
                        for i in 0..4 {
                            if br.read_bool()? {
                                list_start -= 1;
                                coef_list[list_start] = ccoef + i;
                                mode_list[list_start] = 3;
                            } else {
                                let idx = (ccoef + i) as usize;
                                nz_coef_idx[nz_coef_count] = BINK_SCAN[idx];
                                nz_coef_count += 1;
                                block[BINK_SCAN[idx]] = if br.read_bool()? { -mask } else { mask };
                                if masks_count == 0 {
                                    return Ok(());
                                }
                                masks_count -= 1;
                            }
                        }
                    },
                1 => {
                        mode_list[list_pos] = 2;
                        for i in 0..3 {
                            coef_list[list_end] = ccoef + i * 4 + 4;
                            mode_list[list_end] = 2;
                            list_end += 1;
                        }
                    },
                3 => {
                        let idx = ccoef as usize;
                        nz_coef_idx[nz_coef_count] = BINK_SCAN[idx];
                        nz_coef_count += 1;
                        block[BINK_SCAN[idx]] = if br.read_bool()? { -mask } else { mask };
                        coef_list[list_pos] = 0;
                        mode_list[list_pos] = 0;
                        list_pos += 1;
                        if masks_count == 0 {
                            return Ok(());
                        }
                        masks_count -= 1;
                    },
                _ => unreachable!(),
            };
        }
        mask >>= 1;
    }

    Ok(())
}

const BINK_FLAG_ALPHA:  u32 = 0x00100000;
const BINK_FLAG_GRAY:   u32 = 0x00020000;

impl NADecoder for BinkDecoder {
    fn init(&mut self, _supp: &mut NADecoderSupport, info: NACodecInfoRef) -> DecoderResult<()> {
        if let NACodecTypeInfo::Video(vinfo) = info.get_properties() {
            let w = vinfo.get_width();
            let h = vinfo.get_height();

            let edata = info.get_extradata().unwrap();
            validate!(edata.len() >= 8);

            let mut br = MemoryReader::new_read(&edata);
            let magic                   = br.read_u32be()?;
            let flags                   = br.read_u32le()?;

            self.is_ver_b  = (magic & 0xFF) == (b'b' as u32);
            self.is_ver_i  = (magic & 0xFF) >= (b'i' as u32);
            self.has_alpha = (flags & BINK_FLAG_ALPHA) != 0;
            self.is_gray   = (flags & BINK_FLAG_GRAY) != 0;
            self.swap_uv   = (magic & 0xFF) >= (b'h' as u32);
            if self.has_alpha && self.is_gray { return Err(DecoderError::NotImplemented); }

            let aplane = if self.has_alpha { Some(NAPixelChromaton::new(0, 0, false, 8, 0, 3, 1)) } else { None };
            let fmt = if !self.is_gray {
                    NAPixelFormaton::new(ColourModel::YUV(YUVSubmodel::YUVJ),
                                         Some(NAPixelChromaton::new(0, 0, false, 8, 0, 0, 1)),
                                         Some(NAPixelChromaton::new(1, 1, false, 8, 0, 1, 1)),
                                         Some(NAPixelChromaton::new(1, 1, false, 8, 0, 2, 1)),
                                         aplane, None,
                                         0, if self.has_alpha { 4 } else { 3 } )
                } else {
                    NAPixelFormaton::new(ColourModel::YUV(YUVSubmodel::YUVJ),
                                         Some(NAPixelChromaton::new(0, 0, false, 8, 0, 0, 1)),
                                         None, None, None, None, 0, 1)
                };
            let myinfo = NACodecTypeInfo::Video(NAVideoInfo::new(w, h, false, fmt));
            self.info = NACodecInfo::new_ref(info.get_name(), myinfo, info.get_extradata()).into_ref();

            //self.init_bundle_lengths(w.max(8), (w + 7) >> 3);
            self.init_bundle_bufs((w + 7) >> 3, (h + 7) >> 3);

            if self.is_ver_b {
                self.qmat_b.calc_binkb_quants();
            }

            Ok(())
        } else {
            Err(DecoderError::InvalidData)
        }
    }
    fn decode(&mut self, _supp: &mut NADecoderSupport, pkt: &NAPacket) -> DecoderResult<NAFrameRef> {
        let src = pkt.get_buffer();

        let mut br = BitReader::new(&src, BitReaderMode::LE);

        let mut buf;
        self.key_frame = pkt.is_keyframe();
        if self.is_ver_b {
            let bufret = self.hams.clone_ref();
            if let Some(bbuf) = bufret {
                buf = bbuf;
            } else {
                let bufinfo = alloc_video_buffer(self.info.get_properties().get_video_info().unwrap(), 4)?;
                buf = bufinfo.get_vbuf().unwrap();
                self.key_frame = true;
                self.hams.add_frame(buf);
                buf = self.hams.get_output_frame().unwrap();
            }
        } else {
            let bufinfo = alloc_video_buffer(self.info.get_properties().get_video_info().unwrap(), 4)?;
            buf = bufinfo.get_vbuf().unwrap();
        }

        let nplanes = if self.is_gray { 1 } else { 3 };
        if self.has_alpha {
            validate!(!self.is_ver_b);
            if self.is_ver_i {
                br.skip(32)?;
            }
            self.decode_plane(&mut br, nplanes, &mut buf)?;
        }
        if self.is_ver_i {
            br.skip(32)?;
        }
        for plane in 0..nplanes {
            if self.is_ver_b {
                self.decode_plane_binkb(&mut br, plane, &mut buf)?;
            } else {
                let plane_idx = if plane > 0 && self.swap_uv { plane ^ 3 } else { plane };
                self.decode_plane(&mut br, plane_idx, &mut buf)?;
            }
        }
        let bufinfo = NABufferType::Video(buf);
        if !self.is_ver_b {
            self.ips.add_frame(bufinfo.get_vbuf().unwrap());
        }

        let mut frm = NAFrame::new_from_pkt(pkt, self.info.clone(), bufinfo);
        frm.set_frame_type(FrameType::P);
        Ok(frm.into_ref())
    }
    fn flush(&mut self) {
        self.ips.clear();
    }
}

const STRICT_MODE_OPTION: &str = "strict_mode";

const BINK_OPTS: &[NAOptionDefinition] = &[
    NAOptionDefinition {
        name: STRICT_MODE_OPTION, description: "Error out on recoverable errors",
        opt_type: NAOptionDefinitionType::Bool },
];

impl NAOptionHandler for BinkDecoder {
    fn get_supported_options(&self) -> &[NAOptionDefinition] { BINK_OPTS }
    fn set_options(&mut self, options: &[NAOption]) {
        for option in options.iter() {
            if let NAOption { name: STRICT_MODE_OPTION, value: NAValue::Bool(ref bval) } = option {
                self.strict_mode = *bval;
            }
        }
    }
    fn query_option_value(&self, name: &str) -> Option<NAValue> {
        if name == STRICT_MODE_OPTION {
            Some(NAValue::Bool(self.strict_mode))
        } else {
            None
        }
    }
}

pub fn get_decoder() -> Box<dyn NADecoder + Send> {
    Box::new(BinkDecoder::new())
}

#[cfg(test)]
mod test {
    use nihav_core::codecs::RegisteredDecoders;
    use nihav_core::demuxers::RegisteredDemuxers;
    use nihav_codec_support::test::dec_video::*;
    use crate::rad_register_all_decoders;
    use crate::rad_register_all_demuxers;
    #[test]
    fn test_binkvid_b() {
        let mut dmx_reg = RegisteredDemuxers::new();
        rad_register_all_demuxers(&mut dmx_reg);
        let mut dec_reg = RegisteredDecoders::new();
        rad_register_all_decoders(&mut dec_reg);

        // sample: https://samples.mplayerhq.hu/game-formats/bink/bikb/NEW.BIK
        test_decoding("bink", "bink-video", "assets/RAD/NEW.BIK", Some(16), &dmx_reg, &dec_reg,
                      ExpectedTestResult::MD5Frames(vec![
                            [0x00edef7e, 0x7efad3b1, 0x7e8bdd74, 0x3f6b00ba],
                            [0xbc40683f, 0xbeb1c5e4, 0x934777b5, 0x8a8a350d],
                            [0x68b78627, 0x28ceb63d, 0xfdb1171a, 0x23e69d90],
                            [0xc8d907a0, 0xb8d44079, 0x0286336b, 0x996479f3],
                            [0x57bbe4ec, 0xdb8bb9c2, 0x0e6f1fd6, 0xe180125e],
                            [0xd43c2ae0, 0x4010007f, 0x2a6360a1, 0xb5203a05],
                            [0xa883acf6, 0x25843f92, 0x4ced9a46, 0x6d513ad9],
                            [0x959e843f, 0x8d8182b9, 0x3f12d29b, 0x2af8d39f],
                            [0x93840946, 0x1188c6d1, 0xd5499833, 0x62aac0c6],
                            [0x4e5a56a6, 0x21517d9a, 0xbe1f270d, 0xe5621945],
                            [0x1b133742, 0x1eb1bf0a, 0x68cab2e3, 0x92176b5d],
                            [0x0cf78c43, 0x4bc15549, 0x3dd94323, 0x737eaaae],
                            [0xdd731c4a, 0x801453b3, 0xa38bef3e, 0x285cfdfe],
                            [0xe1fec4ee, 0x46737abc, 0x8c452209, 0xc8c6addd],
                            [0x2978aa50, 0x5f1e6d5a, 0x1f5b0fba, 0xb8e32196],
                            [0x2e1e95ab, 0x8e31a0b0, 0xfe998512, 0xea9397b6],
                            [0xf7f6c0d8, 0x893e77a7, 0xdfe0309f, 0xf5e644e2]]));
    }
    #[test]
    fn test_binkvid() {
        let mut dmx_reg = RegisteredDemuxers::new();
        rad_register_all_demuxers(&mut dmx_reg);
        let mut dec_reg = RegisteredDecoders::new();
        rad_register_all_decoders(&mut dec_reg);

        // sample: https://samples.mplayerhq.hu/game-formats/bink/ActivisionLogo.bik
        test_decoding("bink", "bink-video", "assets/RAD/ActivisionLogo.bik", Some(42),
                      &dmx_reg, &dec_reg,
                      ExpectedTestResult::MD5([0x41128884, 0x73a8c710, 0x5072ea4a, 0x8caca428]));
    }
}
