//use std::mem;
use nihav_core::codecs::DecoderError;
use nihav_core::frame::*;
use super::super::*;
use super::super::blockdsp;
use super::*;
use super::code::{H263_INTERP_FUNCS, obmc_filter};
use super::data::H263_CHROMA_ROUND;
use nihav_core::formats;

#[allow(dead_code)]
struct MVInfo {
    mv:         Vec<MV>,
    mb_w:       usize,
    mb_stride:  usize,
    mb_start:   usize,
    top:        bool,
    mvmode:     MVMode,
}

impl MVInfo {
    fn new() -> Self { MVInfo{ mv: Vec::new(), mb_w: 0, mb_stride: 0, mb_start: 0, top: true, mvmode: MVMode::Old } }
    fn reset(&mut self, mb_w: usize, mb_start: usize, mvmode: MVMode) {
        self.mb_start  = mb_start;
        self.mb_w      = mb_w;
        self.mb_stride = mb_w * 2 + 2;
        self.mv.resize(self.mb_stride * 3, ZERO_MV);
        self.mvmode    = mvmode;
    }
    fn update_row(&mut self) {
        self.mb_start = self.mb_w + 1;
        self.top      = false;
        for i in 0..self.mb_stride {
            self.mv[i] = self.mv[self.mb_stride * 2 + i];
        }
    }
    #[allow(non_snake_case)]
    fn predict(&mut self, mb_x: usize, blk_no: usize, use4: bool, diff: MV, first_line: bool, first_mb: bool) -> MV {
        let A;
        let B;
        let C;
        let last = mb_x == self.mb_w - 1;
        match blk_no {
            0 => {
                    if mb_x != self.mb_start {
                        A = if !first_mb   { self.mv[self.mb_stride + mb_x * 2 - 1] } else { ZERO_MV };
                        B = if !first_line { self.mv[                 mb_x * 2] } else { A };
                        C = if !first_line && !last { self.mv[mb_x * 2 + 2] } else { ZERO_MV };
                    } else {
                        A = ZERO_MV; B = ZERO_MV; C = ZERO_MV;
                    }
                },
            1 => {
                    A = self.mv[self.mb_stride + mb_x * 2];
                    B = if !first_line { self.mv[mb_x * 2 + 1] } else { A };
                    C = if !first_line && !last { self.mv[mb_x * 2 + 2] } else { ZERO_MV/*A*/ };
                },
            2 => {
                    A = if mb_x != self.mb_start { self.mv[self.mb_stride * 2 + mb_x * 2 - 1] } else { ZERO_MV };
                    B = self.mv[self.mb_stride + mb_x * 2];
                    C = self.mv[self.mb_stride + mb_x * 2 + 1];
                },
            3 => {
                    A = self.mv[self.mb_stride * 2 + mb_x * 2];
                    B = self.mv[self.mb_stride * 1 + mb_x * 2 + 1];
                    C = self.mv[self.mb_stride * 1 + mb_x * 2];
                },
            _ => { return ZERO_MV; }
        }
        let pred_mv = MV::pred(A, B, C);
        let new_mv = MV::add_umv(pred_mv, diff, self.mvmode);
        if !use4 {
            self.mv[self.mb_stride * 1 + mb_x * 2 + 0] = new_mv;
            self.mv[self.mb_stride * 1 + mb_x * 2 + 1] = new_mv;
            self.mv[self.mb_stride * 2 + mb_x * 2 + 0] = new_mv;
            self.mv[self.mb_stride * 2 + mb_x * 2 + 1] = new_mv;
        } else {
            match blk_no {
                0 => { self.mv[self.mb_stride * 1 + mb_x * 2 + 0] = new_mv; },
                1 => { self.mv[self.mb_stride * 1 + mb_x * 2 + 1] = new_mv; },
                2 => { self.mv[self.mb_stride * 2 + mb_x * 2 + 0] = new_mv; },
                3 => { self.mv[self.mb_stride * 2 + mb_x * 2 + 1] = new_mv; },
                _ => {},
            };
        }

        new_mv
    }
    fn set_zero_mv(&mut self, mb_x: usize) {
        self.mv[self.mb_stride * 1 + mb_x * 2 + 0] = ZERO_MV;
        self.mv[self.mb_stride * 1 + mb_x * 2 + 1] = ZERO_MV;
        self.mv[self.mb_stride * 2 + mb_x * 2 + 0] = ZERO_MV;
        self.mv[self.mb_stride * 2 + mb_x * 2 + 1] = ZERO_MV;
    }
    fn get_mv(&self, mb_x: usize, blk_no: usize) -> MV {
        self.mv[self.mb_stride + mb_x * 2 + (blk_no & 1) + (blk_no >> 1) * self.mb_stride]
    }
}

#[allow(dead_code)]
#[derive(Clone,Copy)]
struct BMB {
    num_mv: usize,
    mv_f:   [MV; 4],
    mv_b:   [MV; 4],
    fwd:    bool,
    blk:    [[i16; 64]; 6],
    cbp:    u8,
}

impl BMB {
    fn new() -> Self { BMB {blk: [[0; 64]; 6], cbp: 0, fwd: false, mv_f: [ZERO_MV; 4], mv_b: [ZERO_MV; 4], num_mv: 0} }
}

#[derive(Clone,Copy)]
struct PredCoeffs {
    hor: [[i16; 8]; 6],
    ver: [[i16; 8]; 6],
}

const ZERO_PRED_COEFFS: PredCoeffs = PredCoeffs { hor: [[1024, 0, 0, 0, 0, 0, 0, 0]; 6], ver: [[1024, 0, 0, 0, 0, 0, 0, 0]; 6] };
const ZERO_PRED_COEFFS2: PredCoeffs = PredCoeffs { hor: [[0x7FFF, 0, 0, 0, 0, 0, 0, 0]; 6], ver: [[0x7FFF, 0, 0, 0, 0, 0, 0, 0]; 6] };

pub const H263DEC_OPT_USES_GOB: u32     = 0x0001;
pub const H263DEC_OPT_SLICE_RESET: u32  = 0x0002;
pub const H263DEC_OPT_HAS_B_FRAMES: u32 = 0x0004;
pub const H263DEC_OPT_HAS_OBMC: u32     = 0x0008;
pub const H263DEC_OPT_PRED_QUANT: u32   = 0x0010;

pub struct H263BaseDecoder {
    w:          usize,
    h:          usize,
    mb_w:       usize,
    mb_h:       usize,
    num_mb:     usize,
    ftype:      Type,
    ipbs:       IPBShuffler,
    next_ts:    u16,
    last_ts:    u16,
    tsdiff:     u16,
    has_b:      bool,
    b_data:     Vec<BMB>,
    pred_coeffs: Vec<PredCoeffs>,
    is_gob:     bool,
    slice_reset: bool,
    may_have_b_frames: bool,
    has_obmc:   bool,
    mv_data:    Vec<BlockMVInfo>,
    blk:        [[i16; 64]; 6],
    obmc_buf:   NAVideoBufferRef<u8>,
    obmc_blk:   Vec<(Type, BMB)>,
    pred_quant: bool,
}

#[inline]
fn clip_dc(dc: i16) -> i16 {
    if dc <= 0 { 0 }
    else if dc > 2046 { 2047 }
    else { dc | 1 }
}

#[inline]
fn clip_ac(ac: i16) -> i16 {
    if ac < -2048 { -2048 }
    else if ac > 2047 { 2047 }
    else { ac }
}

#[allow(dead_code)]
impl H263BaseDecoder {
    pub fn new_with_opts(options: u32) -> Self {
        let is_gob              = (options & H263DEC_OPT_USES_GOB) != 0;
        let slice_reset         = (options & H263DEC_OPT_SLICE_RESET) != 0;
        let may_have_b_frames   = (options & H263DEC_OPT_HAS_B_FRAMES) != 0;
        let has_obmc            = (options & H263DEC_OPT_HAS_OBMC) != 0;
        let pred_quant          = (options & H263DEC_OPT_PRED_QUANT) != 0;

        let vbuf = alloc_video_buffer(NAVideoInfo::new(64, 64, false, YUV420_FORMAT), 4).unwrap();
        let obmc_buf = vbuf.get_vbuf().unwrap();

        H263BaseDecoder{
            w: 0, h: 0, mb_w: 0, mb_h: 0, num_mb: 0,
            ftype: Type::Special,
            ipbs: IPBShuffler::new(),
            last_ts: 0, next_ts: 0, tsdiff: 0,
            has_b: false, b_data: Vec::new(),
            pred_coeffs: Vec::new(),
            is_gob, slice_reset,
            may_have_b_frames, has_obmc,
            pred_quant,
            mv_data: Vec::new(),
            blk: [[0; 64]; 6],
            obmc_buf,
            obmc_blk: Vec::new(),
        }
    }
    pub fn new(is_gob: bool) -> Self {
        Self::new_with_opts(H263DEC_OPT_SLICE_RESET | (if is_gob { H263DEC_OPT_USES_GOB } else { 0 }))
    }
    pub fn new_b_frames(is_gob: bool) -> Self {
        Self::new_with_opts(H263DEC_OPT_SLICE_RESET | H263DEC_OPT_HAS_B_FRAMES | (if is_gob { H263DEC_OPT_USES_GOB } else { 0 }))
    }

    pub fn is_intra(&self) -> bool { self.ftype == Type::I }
    pub fn get_frame_type(&self) -> FrameType {
        match self.ftype {
            Type::I       => FrameType::I,
            Type::P       => FrameType::P,
            Type::B       => FrameType::B,
            Type::PB      => FrameType::P,
            Type::Skip    => FrameType::Skip,
            Type::Special => FrameType::Skip,
        }
    }
    pub fn get_dimensions(&self) -> (usize, usize) { (self.w, self.h) }

    fn decode_intra_mb(&mut self, bd: &mut dyn BlockDecoder, bdsp: &dyn BlockDSP, mb_pos: usize, binfo: &BlockInfo, sstate: &SliceState, apply_acpred: bool) -> DecoderResult<()> {
        for i in 0..6 {
            bd.decode_block_intra(&binfo, &sstate, binfo.get_q(), i, (binfo.cbp & (1 << (5 - i))) != 0, &mut self.blk[i])?;
            if apply_acpred && (binfo.acpred != ACPredMode::None) {
                let has_b = (i == 1) || (i == 3) || !sstate.first_mb;
                let has_a = (i == 2) || (i == 3) || !sstate.first_line;
                let (b_mb, b_blk) = if has_b {
                        if (i == 1) || (i == 3) {
                            (mb_pos, i - 1)
                        } else if i < 4 {
                            (mb_pos - 1, i + 1)
                        } else {
                            (mb_pos - 1, i)
                        }
                    } else { (0, 0) };
                let (a_mb, a_blk) = if has_a {
                        if (i == 2) || (i == 3) {
                            (mb_pos, i - 2)
                        } else if i < 4 {
                            (mb_pos - self.mb_w, i + 2)
                        } else {
                            (mb_pos - self.mb_w, i)
                        }
                    } else { (0, 0) };
                match binfo.acpred {
                    ACPredMode::DC   => {
                                let dc;
                                if has_a && has_b {
                                    dc = (self.pred_coeffs[b_mb].hor[b_blk][0] + self.pred_coeffs[a_mb].ver[a_blk][0]) / 2;
                                } else if has_a {
                                    dc = self.pred_coeffs[a_mb].ver[a_blk][0];
                                } else if has_b {
                                    dc = self.pred_coeffs[b_mb].hor[b_blk][0];
                                } else {
                                    dc = 1024;
                                }
                                self.blk[i][0] = clip_dc(self.blk[i][0] + dc);
                            },
                    ACPredMode::Hor  => {
                            if has_b {
                                for k in 0..8 {
                                    self.blk[i][k * 8] += self.pred_coeffs[b_mb].hor[b_blk][k];
                                }
                                for k in 1..8 {
                                    self.blk[i][k * 8] = clip_ac(self.blk[i][k * 8]);
                                }
                            } else {
                                self.blk[i][0] += 1024;
                            }
                            self.blk[i][0] = clip_dc(self.blk[i][0]);
                        },
                    ACPredMode::Ver  => {
                            if has_a {
                                for k in 0..8 {
                                    self.blk[i][k] += self.pred_coeffs[a_mb].ver[a_blk][k];
                                }
                                for k in 1..8 {
                                    self.blk[i][k] = clip_ac(self.blk[i][k]);
                                }
                            } else {
                                self.blk[i][0] += 1024;
                            }
                            self.blk[i][0] = clip_dc(self.blk[i][0]);
                        },
                    ACPredMode::None => {},
                };
                for t in 0..8 { self.pred_coeffs[mb_pos].hor[i][t] = self.blk[i][t * 8]; }
                for t in 0..8 { self.pred_coeffs[mb_pos].ver[i][t] = self.blk[i][t]; }
            }
            bdsp.idct(&mut self.blk[i]);
        }
        Ok(())
    }
    #[allow(clippy::comparison_chain)]
    fn decode_intra_mb_pred_quant(&mut self, bd: &mut dyn BlockDecoder, bdsp: &dyn BlockDSP, mb_pos: usize, binfo: &BlockInfo, sstate: &SliceState, apply_acpred: bool) -> DecoderResult<()> {
        for i in 0..6 {
            bd.decode_block_intra(&binfo, &sstate, binfo.get_q(), i, (binfo.cbp & (1 << (5 - i))) != 0, &mut self.blk[i])?;
            let q = if binfo.get_q() < 8 {
                    8
                } else {
                    i16::from(binfo.get_q() * 2)
                };
            let qadd = (q + 1) >> 1;
            let quant_gray = 1024 / q;
            if apply_acpred && (binfo.acpred != ACPredMode::None) {
                let has_b = (i == 1) || (i == 3) || !sstate.first_mb;
                let has_a = (i == 2) || (i == 3) || !sstate.first_line;
                let (b_mb, b_blk) = if has_b {
                        if (i == 1) || (i == 3) {
                            (mb_pos, i - 1)
                        } else if i < 4 {
                            (mb_pos - 1, i + 1)
                        } else {
                            (mb_pos - 1, i)
                        }
                    } else { (0, 0) };
                let (a_mb, a_blk) = if has_a {
                        if (i == 2) || (i == 3) {
                            (mb_pos, i - 2)
                        } else if i < 4 {
                            (mb_pos - self.mb_w, i + 2)
                        } else {
                            (mb_pos - self.mb_w, i)
                        }
                    } else { (0, 0) };
                let dc_a = if has_a && self.pred_coeffs[a_mb].ver[a_blk][0] != 0x7FFF {
                        self.pred_coeffs[a_mb].ver[a_blk][0]
                    } else {
                        quant_gray
                    };
                let dc_b = if has_b && self.pred_coeffs[b_mb].hor[b_blk][0] != 0x7FFF {
                        self.pred_coeffs[b_mb].hor[b_blk][0]
                    } else {
                        quant_gray
                    };
                let dc = match binfo.acpred {
                    ACPredMode::DC   => {
                                if has_a && has_b {
                                    (dc_a + dc_b + 1) >> 1
                                } else if has_a {
                                    dc_a
                                } else if has_b {
                                    dc_b
                                } else {
                                    quant_gray
                                }
                            },
                    ACPredMode::Hor  => {
                            if has_b {
                                for k in 1..8 {
                                    self.blk[i][k * 8] += self.pred_coeffs[b_mb].hor[b_blk][k];
                                }
                                for k in 1..8 {
                                    self.blk[i][k * 8] = clip_ac(self.blk[i][k * 8]);
                                }
                                dc_b
                            } else {
                                quant_gray
                            }
                        },
                    ACPredMode::Ver  => {
                            if has_a {
                                for k in 1..8 {
                                    self.blk[i][k] += self.pred_coeffs[a_mb].ver[a_blk][k];
                                }
                                for k in 1..8 {
                                    self.blk[i][k] = clip_ac(self.blk[i][k]);
                                }
                                dc_a
                            } else {
                                quant_gray
                            }
                        },
                    ACPredMode::None => { 0 },
                };
                self.blk[i][0] += dc;
                for t in 0..8 { self.pred_coeffs[mb_pos].hor[i][t] = self.blk[i][t * 8]; }
                for t in 0..8 { self.pred_coeffs[mb_pos].ver[i][t] = self.blk[i][t]; }
            }
            if apply_acpred {
                let start = if binfo.get_q() < 8 {
                        self.blk[i][0] <<= 3;
                        1
                    } else {
                        0
                    };
                for el in self.blk[i].iter_mut().skip(start) {
                    if *el > 0 {
                        *el = *el * q + qadd;
                    } else if *el < 0 {
                        *el = *el * q - qadd;
                    }
                }
            }
            bdsp.idct(&mut self.blk[i]);
        }
        Ok(())
    }
    fn decode_inter_mb(&mut self, bd: &mut dyn BlockDecoder, bdsp: &dyn BlockDSP, binfo: &BlockInfo, sstate: &SliceState) -> DecoderResult<()> {
        for i in 0..6 {
            bd.decode_block_inter(&binfo, &sstate, binfo.get_q(), i, ((binfo.cbp >> (5 - i)) & 1) != 0, &mut self.blk[i])?;
            bdsp.idct(&mut self.blk[i]);
        }
        Ok(())
    }
    fn get_obmc_mv(&self, mb_idx: usize, blk_no: usize, cur_mv: MV) -> MV {
        let (mbt, ref bi) = self.obmc_blk[mb_idx];
        if mbt == Type::I {
            cur_mv
        } else if mbt == Type::Skip {
            ZERO_MV
        } else if bi.num_mv == 1 {
            bi.mv_f[0]
        } else {
            bi.mv_f[blk_no]
        }
    }
    #[allow(clippy::cognitive_complexity)]
    fn reconstruct_obmc(&mut self, buf: &mut NAVideoBuffer<u8>, slice_start: usize, start: usize, end: usize, slice_end: bool) -> usize {
        let mut mb_x = start % self.mb_w;
        let mut mb_y = start / self.mb_w;
        let mut mb_pos = start;
        while mb_pos < end {
            let has_top  = mb_pos >= slice_start + self.mb_w;
            let has_left = (mb_x > 0) && (mb_pos > slice_start);
            let has_right = (mb_x + 1 < self.mb_w) && (mb_pos + 1 < end);
            let has_bottom = mb_pos + self.mb_w < end;

            if !has_bottom && !slice_end {
                break;
            }

            let (mbt, ref bi) = self.obmc_blk[mb_pos];
            if mbt == Type::P || mbt == Type::Skip {
                let single_mv = bi.num_mv != 4;
                for blk in 0..4 {
                    let cur_mv = if single_mv { bi.mv_f[0] } else { bi.mv_f[blk] };
                    let top_mv = if (blk & 2) == 0 {
                            if has_top {
                                self.get_obmc_mv(mb_pos - self.mb_w, blk + 2, cur_mv)
                            } else { cur_mv }
                        } else {
                            if single_mv { cur_mv } else { bi.mv_f[blk - 2] }
                        };
                    let left_mv = if (blk & 1) == 0 {
                            if has_left {
                                self.get_obmc_mv(mb_pos - 1, blk + 1, cur_mv)
                            } else { cur_mv }
                        } else {
                            if single_mv { cur_mv } else { bi.mv_f[blk - 1] }
                        };
                    let bottom_mv = if (blk & 2) != 0 {
                            if has_bottom {
                                self.get_obmc_mv(mb_pos + self.mb_w, blk - 2, cur_mv)
                            } else { cur_mv }
                        } else {
                            if single_mv { cur_mv } else { bi.mv_f[blk + 2] }
                        };
                    let right_mv = if (blk & 1) != 0 {
                            if has_right {
                                self.get_obmc_mv(mb_pos + 1, blk - 1, cur_mv)
                            } else { cur_mv }
                        } else {
                            if single_mv { cur_mv } else { bi.mv_f[blk + 1] }
                        };

                    let mut obmcbuf = NASimpleVideoFrame::from_video_buf(&mut self.obmc_buf).unwrap();

                    if let Some(ref srcbuf) = self.ipbs.get_lastref() {
                        let dx = (mb_x * 16 + (blk & 1) * 8) as i16;
                        let dy = (mb_y * 16 + (blk & 2) * 4) as i16;

                        let block_params = [(8, 0, top_mv), (0, 8, left_mv), (8, 8, cur_mv), (16, 8, right_mv), (8, 16, bottom_mv)];

                        for (off_x, off_y, mv) in block_params.iter() {
                            let mx = dx + (mv.x >> 1) - off_x;
                            let my = dy + (mv.y >> 1) - off_y;
                            let mode = ((mv.x & 1) + (mv.y & 1) * 2) as usize;
                            blockdsp::copy_block(&mut obmcbuf, srcbuf.clone(), 0,
                                                 *off_x as usize, *off_y as usize, mx, my,
                                                 8, 8, 0, 1, mode, H263_INTERP_FUNCS);
                        }
                        let stride = buf.get_stride(0);
                        let off = buf.get_offset(0) + (dx as usize) + (dy as usize) * stride;
                        let data = buf.get_data_mut().unwrap();
                        obmc_filter(&mut data[off..], stride, obmcbuf.data, obmcbuf.stride[0]);
                    }
                }
                if let Some(ref srcbuf) = self.ipbs.get_lastref() {
                    let mut dst = NASimpleVideoFrame::from_video_buf(buf).unwrap();
                    let xpos = mb_x * 8;
                    let ypos = mb_y * 8;

                    let (mvx, mvy, cmode) = if single_mv {
                            let mv = bi.mv_f[0];
                            let cmode = (if (mv.x & 3) != 0 { 1 } else { 0 }) + (if (mv.y & 3) != 0 { 2 } else { 0 });
                            (mv.x >> 2, mv.y >> 2, cmode)
                        } else {
                            let sum_mv = bi.mv_f[0] + bi.mv_f[1] + bi.mv_f[2] + bi.mv_f[3];
                            let cmx = (sum_mv.x >> 3) + H263_CHROMA_ROUND[(sum_mv.x & 0xF) as usize];
                            let cmy = (sum_mv.y >> 3) + H263_CHROMA_ROUND[(sum_mv.y & 0xF) as usize];
                            let cmode = ((cmx & 1) + (cmy & 1) * 2) as usize;
                            (cmx, cmy, cmode)
                        };
                    blockdsp::copy_block(&mut dst, srcbuf.clone(), 1, xpos, ypos, mvx, mvy, 8, 8, 0, 1, cmode, H263_INTERP_FUNCS);
                    blockdsp::copy_block(&mut dst, srcbuf.clone(), 2, xpos, ypos, mvx, mvy, 8, 8, 0, 1, cmode, H263_INTERP_FUNCS);
                }
                if mbt != Type::Skip {
                    blockdsp::add_blocks(buf, mb_x, mb_y, &bi.blk);
                }
            }

            mb_pos += 1;
            mb_x   += 1;
            if mb_x == self.mb_w {
                mb_x = 0;
                mb_y += 1;
            }
        }
        mb_pos
    }
    #[allow(clippy::cognitive_complexity)]
    pub fn parse_frame(&mut self, bd: &mut dyn BlockDecoder, bdsp: &dyn BlockDSP) -> DecoderResult<NABufferType> {
        let pinfo = bd.decode_pichdr()?;
        let mut mvi = MVInfo::new();
        let mut mvi2 = MVInfo::new();
        let mut cbpi = CBPInfo::new();

//todo handle res change
        self.w = pinfo.w;
        self.h = pinfo.h;
        self.mb_w = (pinfo.w + 15) >> 4;
        self.mb_h = (pinfo.h + 15) >> 4;
        self.num_mb = self.mb_w * self.mb_h;
        self.ftype = pinfo.mode;
        self.has_b = pinfo.is_pb();

        let do_obmc = self.has_obmc && (pinfo.mode == Type::P);

        if do_obmc {
            let capacity = self.obmc_blk.capacity();
            if capacity < self.num_mb {
                self.obmc_blk.reserve(self.num_mb - capacity);
            }
            self.obmc_blk.clear();
        }

        if self.has_b {
            self.mv_data.clear();
        }

        let save_b_data = pinfo.mode.is_ref() && self.may_have_b_frames;
        if save_b_data {
            self.mv_data.clear();
        }
        let is_b = pinfo.mode == Type::B;

        if is_b && (self.mv_data.len() < self.mb_w * self.mb_h) {
            return Err(DecoderError::MissingReference);
        }

        let tsdiff = if pinfo.is_pb() { pinfo.ts.wrapping_sub(self.last_ts) >> 1 }
                     else { self.last_ts.wrapping_sub(self.next_ts) >> 1 };
        let bsdiff = if pinfo.is_pb() { (pinfo.get_pbinfo().get_trb() as u16) << 7 }
                     else { pinfo.ts.wrapping_sub(self.next_ts) >> 1 };

        let fmt = formats::YUV420_FORMAT;
        let vinfo = NAVideoInfo::new(self.w, self.h, false, fmt);
        let bufinfo = alloc_video_buffer(vinfo, 4)?;
        let mut buf = bufinfo.get_vbuf().unwrap();

        let mut slice = if self.is_gob {
                SliceInfo::get_default_slice(&pinfo)
            } else {
                bd.decode_slice_header(&pinfo)?
            };
        mvi.reset(self.mb_w, 0, pinfo.get_mvmode());
        if is_b || pinfo.is_pb() {
            mvi2.reset(self.mb_w, 0, pinfo.get_mvmode());
        }
        cbpi.reset(self.mb_w);

        let mut sstate = SliceState::new(pinfo.mode == Type::I);
        let mut mb_pos = 0;
        let apply_acpred = /*(pinfo.mode == Type::I) && */pinfo.plusinfo.is_some() && pinfo.plusinfo.unwrap().aic;
        if apply_acpred {
            self.pred_coeffs.clear();
            if !self.pred_quant {
                self.pred_coeffs.resize(self.mb_w * self.mb_h, ZERO_PRED_COEFFS);
            } else {
                self.pred_coeffs.resize(self.mb_w * self.mb_h, ZERO_PRED_COEFFS2);
            }
        }
        sstate.quant = slice.quant;
        let mut obmc_start = 0;
        let mut slice_start = 0;
        for mb_y in 0..self.mb_h {
            for mb_x in 0..self.mb_w {
                self.blk = [[0; 64]; 6];

                if slice.is_at_end(mb_pos) || (slice.needs_check() && mb_pos > 0 && bd.is_slice_end()) {
                    if do_obmc {
                        self.reconstruct_obmc(&mut buf, slice_start, obmc_start, mb_pos, true);
                        obmc_start = mb_pos;
                        slice_start = mb_pos;
                    }
                    slice = bd.decode_slice_header(&pinfo)?;
                    if self.is_gob || self.slice_reset {
                        mvi.reset(self.mb_w, mb_x, pinfo.get_mvmode());
                        if is_b || pinfo.is_pb() {
                            mvi2.reset(self.mb_w, mb_x, pinfo.get_mvmode());
                        }
                        cbpi.reset(self.mb_w);
                        sstate.reset_slice(mb_x, mb_y);
                        sstate.quant = slice.quant;
                    }
                }

                let binfo = bd.decode_block_header(&pinfo, &slice, &sstate)?;
                let cbp = binfo.get_cbp();
                cbpi.set_cbp(mb_x, cbp);
                cbpi.set_q(mb_x, binfo.get_q());
                sstate.quant = binfo.get_q();
                if binfo.is_intra() {
                    if save_b_data {
                        self.mv_data.push(BlockMVInfo::Intra);
                    }
                    if !self.pred_quant {
                        self.decode_intra_mb(bd, bdsp, mb_pos, &binfo, &sstate, apply_acpred)?;
                    } else {
                        self.decode_intra_mb_pred_quant(bd, bdsp, mb_pos, &binfo, &sstate, apply_acpred)?;
                    }
                    blockdsp::put_blocks(&mut buf, mb_x, mb_y, &self.blk);
                    mvi.set_zero_mv(mb_x);
                    if is_b {
                        mvi2.set_zero_mv(mb_x);
                    } else if pinfo.is_pb() {
                        mvi.predict(mb_x, 0, false, binfo.get_mv2(0), sstate.first_line, sstate.first_mb);
                        mvi2.predict(mb_x, 0, false, binfo.get_mv2(0), sstate.first_line, sstate.first_mb);
                    }
                    if do_obmc {
//todo: use MV from PB-part if available
                        self.obmc_blk.push((Type::I, BMB::new()));
                    }
                } else if (binfo.mode != Type::B) && !binfo.is_skipped() {
                    if binfo.get_num_mvs() == 1 {
                        let mv = mvi.predict(mb_x, 0, false, binfo.get_mv(0), sstate.first_line, sstate.first_mb);
                        if save_b_data {
                            self.mv_data.push(BlockMVInfo::Inter_1MV(mv));
                        }
                        if let Some(ref srcbuf) = self.ipbs.get_lastref() {
                            bdsp.copy_blocks(&mut buf, srcbuf.clone(), mb_x * 16, mb_y * 16, mv);
                        }
                        if pinfo.is_pb() {
                            mvi2.predict(mb_x, 0, false, binfo.get_mv(0), sstate.first_line, sstate.first_mb);
                        }
                        if do_obmc {
                            let mut blki = BMB::new();
                            blki.mv_f[0] = mv;
                            blki.num_mv  = 1;
                            self.obmc_blk.push((Type::P, blki));
                        }
                    } else {
                        let mut mv: [MV; 4] = [ZERO_MV, ZERO_MV, ZERO_MV, ZERO_MV];
                        for blk_no in 0..4 {
                            mv[blk_no] = mvi.predict(mb_x, blk_no, true, binfo.get_mv(blk_no), sstate.first_line, sstate.first_mb);
                        }
                        if let Some(ref srcbuf) = self.ipbs.get_lastref() {
                            bdsp.copy_blocks8x8(&mut buf, srcbuf.clone(), mb_x * 16, mb_y * 16, &mv);
                        }
                        if pinfo.is_pb() {
                            for blk_no in 0..4 {
                                mvi2.predict(mb_x, blk_no, true, binfo.get_mv(blk_no), sstate.first_line, sstate.first_mb);
                            }
                        }
                        if save_b_data {
                            self.mv_data.push(BlockMVInfo::Inter_4MV(mv));
                        }
                        if do_obmc {
                            let mut blki = BMB::new();
                            blki.mv_f   = mv;
                            blki.num_mv = 4;
                            self.obmc_blk.push((Type::P, blki));
                        }
                    }
                    self.decode_inter_mb(bd, bdsp, &binfo, &sstate)?;
                    if !do_obmc {
                        blockdsp::add_blocks(&mut buf, mb_x, mb_y, &self.blk);
                    } else {
                        let (_, ref mut bi) = self.obmc_blk[mb_pos];
                        bi.blk = self.blk;
                    }
                    if is_b && !pinfo.is_pb() {
                        mvi2.set_zero_mv(mb_x);
                    }
                } else if binfo.mode != Type::B {
                    self.mv_data.push(BlockMVInfo::Inter_1MV(ZERO_MV));
                    mvi.set_zero_mv(mb_x);
                    if is_b || pinfo.is_pb() {
                        mvi2.set_zero_mv(mb_x);
                    }
                    if !do_obmc {
                        if let Some(ref srcbuf) = self.ipbs.get_lastref() {
                            bdsp.copy_blocks(&mut buf, srcbuf.clone(), mb_x * 16, mb_y * 16, ZERO_MV);
                        }
                    } else {
                        self.obmc_blk.push((Type::Skip, BMB::new()));
                    }
                } else {
                    recon_b_mb(&mut buf, &mut self.ipbs, bdsp, &mut mvi, &mut mvi2, mb_pos, self.mb_w, &sstate, &binfo, &self.mv_data, bsdiff, tsdiff);
                    if cbp != 0 {
                        self.decode_inter_mb(bd, bdsp, &binfo, &sstate)?;
                        blockdsp::add_blocks(&mut buf, mb_x, mb_y, &self.blk);
                    }
                }
                if pinfo.is_pb() {
                    let mut b_mb = BMB::new();
                    let cbp = binfo.get_cbp_b();
                    let bq = (((pinfo.get_pbinfo().get_dbquant() + 5) as u16) * (binfo.get_q() as u16)) >> 2;
                    let bquant;
                    if bq < 1 { bquant = 1; }
                    else if bq > 31 { bquant = 31; }
                    else { bquant = bq as u8; }

                    b_mb.cbp = cbp;
                    for i in 0..6 {
                        bd.decode_block_inter(&binfo, &sstate, bquant, i, (cbp & (1 << (5 - i))) != 0, &mut b_mb.blk[i])?;
                        bdsp.idct(&mut b_mb.blk[i]);
                    }

                    let is_fwd = binfo.is_b_fwd();
                    b_mb.fwd = is_fwd;
                    if binfo.get_num_mvs() != 4 {
                        let ref_mv = mvi2.get_mv(mb_x, 0);
                        let b_mv = if binfo.is_intra() { binfo.get_mv2(1) } else { binfo.get_mv2(0) };
                        let src_mv = if is_fwd { ZERO_MV } else { ref_mv.scale(bsdiff, tsdiff) };
                        let mv_f = MV::add_umv(src_mv, b_mv, pinfo.get_mvmode());
                        let mv_b = MV::b_sub(ref_mv, mv_f, b_mv, bsdiff, tsdiff);
                        b_mb.mv_f[0] = mv_f;
                        b_mb.mv_b[0] = mv_b;
                        b_mb.num_mv = 1;
                    } else {
                        for blk_no in 0..4 {
                            let src_mv = if is_fwd { ZERO_MV } else { mvi2.get_mv(mb_x, blk_no).scale(bsdiff, tsdiff) };
                            let mv_f = MV::add_umv(src_mv, binfo.get_mv2(0), pinfo.get_mvmode());
                            let mv_b = MV::b_sub(mvi2.get_mv(mb_x, blk_no), mv_f, binfo.get_mv2(0), bsdiff, tsdiff);
                            b_mb.mv_f[blk_no] = mv_f;
                            b_mb.mv_b[blk_no] = mv_b;
                        }
                        b_mb.num_mv = 4;
                    }
                    self.b_data.push(b_mb);
                }
                sstate.next_mb();
                mb_pos += 1;
            }
            if do_obmc && (mb_pos > obmc_start + self.mb_w) {
                obmc_start = self.reconstruct_obmc(&mut buf, slice_start, obmc_start, mb_pos - self.mb_w, false);
            } else if let Some(plusinfo) = pinfo.plusinfo {
                if plusinfo.deblock {
                    bdsp.filter_row(&mut buf, mb_y, self.mb_w, &cbpi);
                }
            }
            mvi.update_row();
            if is_b || pinfo.is_pb() {
                mvi2.update_row();
            }
            cbpi.update_row();
            sstate.new_row();
        }
        if do_obmc {
            self.reconstruct_obmc(&mut buf, slice_start, obmc_start, mb_pos, true);
        }

        if pinfo.mode.is_ref() {
            self.ipbs.add_frame(buf);
            self.next_ts = self.last_ts;
            self.last_ts = pinfo.ts;
            self.tsdiff  = tsdiff;
        }

        Ok(bufinfo)
    }
    pub fn flush(&mut self) {
        self.ipbs.clear();
    }

    pub fn get_bframe(&mut self, bdsp: &dyn BlockDSP) -> DecoderResult<NABufferType> {
        if !self.has_b || self.ipbs.get_lastref().is_none() || self.ipbs.get_nextref().is_none() {
            return Err(DecoderError::MissingReference);
        }
        self.has_b = false;

        let fmt = formats::YUV420_FORMAT;
        let vinfo = NAVideoInfo::new(self.w, self.h, false, fmt);
        let bufinfo = alloc_video_buffer(vinfo, 4)?;
        let b_buf = bufinfo.get_vbuf().unwrap();

        if let (Some(ref bck_buf), Some(ref fwd_buf)) = (self.ipbs.get_b_bwdref(), self.ipbs.get_b_fwdref()) {
            recon_b_frame(b_buf, bck_buf.clone(), fwd_buf.clone(), self.mb_w, self.mb_h, self.b_data.as_slice(), bdsp);
        }

        self.b_data.clear();
        Ok(bufinfo)
    }
}

fn recon_b_mb(buf: &mut NAVideoBuffer<u8>, ipbs: &mut IPBShuffler, bdsp: &dyn BlockDSP, mvi: &mut MVInfo, mvi2: &mut MVInfo, mb_pos: usize, mb_w: usize, sstate: &SliceState, binfo: &BlockInfo, mv_data: &[BlockMVInfo], bsdiff: u16, tsdiff: u16) {
    let mb_x = mb_pos % mb_w;
    let mb_y = mb_pos / mb_w;

    let ref_mv_info = mv_data[mb_pos];
    let has_fwd = binfo.get_num_mvs() > 0;
    let has_bwd = binfo.get_num_mvs2() > 0;

    if has_fwd || has_bwd {
        let fwd_mv;
        if has_fwd {
            fwd_mv = mvi.predict(mb_x, 0, false, binfo.get_mv(0), sstate.first_line, sstate.first_mb);
        } else {
            fwd_mv = ZERO_MV;
            mvi.set_zero_mv(mb_x);
        }
        let bwd_mv;
        if has_bwd {
            bwd_mv = mvi2.predict(mb_x, 0, false, binfo.get_mv2(0), sstate.first_line, sstate.first_mb);
        } else {
            bwd_mv = ZERO_MV;
            mvi2.set_zero_mv(mb_x);
        }
        if let (Some(ref fwd_buf), Some(ref bck_buf)) = (ipbs.get_nextref(), ipbs.get_lastref()) {
            if has_fwd && has_bwd {
                bdsp.copy_blocks(buf, fwd_buf.clone(), mb_x * 16, mb_y * 16, fwd_mv);
                bdsp.avg_blocks (buf, bck_buf.clone(), mb_x * 16, mb_y * 16, bwd_mv);
            } else if has_fwd {
                bdsp.copy_blocks(buf, fwd_buf.clone(), mb_x * 16, mb_y * 16, fwd_mv);
            } else {
                bdsp.copy_blocks(buf, bck_buf.clone(), mb_x * 16, mb_y * 16, bwd_mv);
            }
        }
    } else {
        if let BlockMVInfo::Inter_4MV(mvs) = ref_mv_info {
            let mut mv_f = [ZERO_MV; 4];
            let mut mv_b = [ZERO_MV; 4];
            for blk_no in 0..4 {
                let ref_mv = mvs[blk_no];
                let ref_mv_fwd = ref_mv.scale(bsdiff, tsdiff);
                let ref_mv_bwd = ref_mv - ref_mv_fwd;
                mv_f[blk_no] = ref_mv_fwd;
                mv_b[blk_no] = ref_mv_bwd;
            }
            if let (Some(ref fwd_buf), Some(ref bck_buf)) = (ipbs.get_nextref(), ipbs.get_lastref()) {
                bdsp.copy_blocks8x8(buf, fwd_buf.clone(), mb_x * 16, mb_y * 16, &mv_f);
                bdsp.avg_blocks8x8 (buf, bck_buf.clone(), mb_x * 16, mb_y * 16, &mv_b);
            }
        } else {
            let ref_mv = if let BlockMVInfo::Inter_1MV(mv_) = ref_mv_info { mv_ } else { ZERO_MV };
            let ref_mv_fwd = ref_mv.scale(bsdiff, tsdiff);
            let ref_mv_bwd = MV::b_sub(ref_mv, ref_mv_fwd, ZERO_MV, bsdiff, tsdiff);

            if let (Some(ref fwd_buf), Some(ref bck_buf)) = (ipbs.get_nextref(), ipbs.get_lastref()) {
                bdsp.copy_blocks(buf, fwd_buf.clone(), mb_x * 16, mb_y * 16, ref_mv_fwd);
                bdsp.avg_blocks (buf, bck_buf.clone(), mb_x * 16, mb_y * 16, ref_mv_bwd);
            }
        }
        mvi.set_zero_mv(mb_x);
        mvi2.set_zero_mv(mb_x);
    }
}

fn recon_b_frame(mut b_buf: NAVideoBufferRef<u8>, bck_buf: NAVideoBufferRef<u8>, fwd_buf: NAVideoBufferRef<u8>,
                 mb_w: usize, mb_h: usize, b_data: &[BMB], bdsp: &dyn BlockDSP) {
    let mut cbpi = CBPInfo::new();
    let mut cur_mb = 0;
    cbpi.reset(mb_w);
    for mb_y in 0..mb_h {
        for mb_x in 0..mb_w {
            let num_mv = b_data[cur_mb].num_mv;
            let is_fwd = b_data[cur_mb].fwd;
            let cbp    = b_data[cur_mb].cbp;
            cbpi.set_cbp(mb_x, cbp);
            if num_mv == 1 {
                bdsp.copy_blocks(&mut b_buf, fwd_buf.clone(), mb_x * 16, mb_y * 16, b_data[cur_mb].mv_f[0]);
                if !is_fwd {
                    bdsp.avg_blocks(&mut b_buf, bck_buf.clone(), mb_x * 16, mb_y * 16, b_data[cur_mb].mv_b[0]);
                }
            } else {
                bdsp.copy_blocks8x8(&mut b_buf, fwd_buf.clone(), mb_x * 16, mb_y * 16, &b_data[cur_mb].mv_f);
                if !is_fwd {
                    bdsp.avg_blocks8x8(&mut b_buf, bck_buf.clone(), mb_x * 16, mb_y * 16, &b_data[cur_mb].mv_b);
                }
            }
            if cbp != 0 {
                blockdsp::add_blocks(&mut b_buf, mb_x, mb_y, &b_data[cur_mb].blk);
            }
            cur_mb += 1;
        }
        cbpi.update_row();
    }
}
