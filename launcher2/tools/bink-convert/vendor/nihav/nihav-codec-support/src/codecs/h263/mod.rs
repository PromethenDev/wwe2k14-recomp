use nihav_core::codecs::DecoderResult;
use super::{MV, ZERO_MV};
use nihav_core::frame::{NAVideoBuffer, NAVideoBufferRef};

#[allow(clippy::erasing_op)]
#[allow(clippy::many_single_char_names)]
pub mod code;
pub mod data;
#[allow(clippy::needless_range_loop)]
pub mod decoder;

pub trait BlockDecoder {
    fn decode_pichdr(&mut self) -> DecoderResult<PicInfo>;
    fn decode_slice_header(&mut self, pinfo: &PicInfo) -> DecoderResult<SliceInfo>;
    fn decode_block_header(&mut self, pinfo: &PicInfo, sinfo: &SliceInfo, sstate: &SliceState) -> DecoderResult<BlockInfo>;
    fn decode_block_intra(&mut self, info: &BlockInfo, sstate: &SliceState, quant: u8, no: usize, coded: bool, blk: &mut [i16; 64]) -> DecoderResult<()>;
    fn decode_block_inter(&mut self, info: &BlockInfo, sstate: &SliceState, quant: u8, no: usize, coded: bool, blk: &mut [i16; 64]) -> DecoderResult<()>;
    fn is_slice_end(&mut self) -> bool;
}

pub trait BlockDSP {
    fn idct(&self, blk: &mut [i16; 64]);
    fn copy_blocks(&self, dst: &mut NAVideoBuffer<u8>, src: NAVideoBufferRef<u8>, xpos: usize, ypos: usize, mv: MV);
    fn copy_blocks8x8(&self, dst: &mut NAVideoBuffer<u8>, src: NAVideoBufferRef<u8>, xpos: usize, ypos: usize, mvs: &[MV; 4]);
    fn avg_blocks(&self, dst: &mut NAVideoBuffer<u8>, src: NAVideoBufferRef<u8>, xpos: usize, ypos: usize, mv: MV);
    fn avg_blocks8x8(&self, dst: &mut NAVideoBuffer<u8>, src: NAVideoBufferRef<u8>, xpos: usize, ypos: usize, mvs: &[MV; 4]);
    fn filter_row(&self, buf: &mut NAVideoBuffer<u8>, mb_y: usize, mb_w: usize, cbpi: &CBPInfo);
}

#[allow(dead_code)]
#[derive(Debug,Clone,Copy,PartialEq)]
pub enum Type {
    I, P, PB, Skip, B, Special
}

impl Type {
    pub fn is_ref(self) -> bool {
        match self {
            Type::I | Type::P | Type::PB => true,
            _                            => false,
        }
    }
}

#[allow(dead_code)]
#[derive(Debug,Clone,Copy)]
pub struct PBInfo {
    trb:        u8,
    dbquant:    u8,
    improved:   bool,
}

impl PBInfo {
    pub fn new(trb: u8, dbquant: u8, improved: bool) -> Self {
        PBInfo{ trb, dbquant, improved }
    }
    pub fn get_trb(self) -> u8 { self.trb }
    pub fn get_dbquant(self) -> u8 { self.dbquant }
    pub fn is_improved(self) -> bool { self.improved }
}

#[allow(dead_code)]
#[derive(Debug,Clone,Copy)]
pub struct PicInfo {
    pub w:          usize,
    pub h:          usize,
    pub mode:       Type,
    pub mvmode:     MVMode,
    pub umv:        bool,
    pub apm:        bool,
    pub quant:      u8,
    pub pb:         Option<PBInfo>,
    pub ts:         u16,
    pub plusinfo:   Option<PlusInfo>,
}

#[allow(dead_code)]
impl PicInfo {
    pub fn new(w: usize, h: usize, mode: Type, mvmode: MVMode, umv: bool, apm: bool, quant: u8, ts: u16, pb: Option<PBInfo>, plusinfo: Option<PlusInfo>) -> Self {
        PicInfo {
            w, h, mode, mvmode,
            umv, apm, quant,
            pb, ts, plusinfo
        }
    }
    pub fn get_width(&self) -> usize { self.w }
    pub fn get_height(&self) -> usize { self.h }
    pub fn get_mode(&self) -> Type { self.mode }
    pub fn get_quant(&self) -> u8 { self.quant }
    pub fn get_apm(&self) -> bool { self.apm }
    pub fn is_pb(&self) -> bool { self.pb.is_some() }
    pub fn is_ipb(&self) -> bool {
            if let Some(ref pbi) = self.pb {
                pbi.is_improved()
            } else {
                false
            }
        }
    pub fn get_ts(&self) -> u16 { self.ts }
    pub fn get_pbinfo(&self) -> PBInfo { self.pb.unwrap() }
    pub fn get_plusifo(&self) -> Option<PlusInfo> { self.plusinfo }
    pub fn get_mvmode(&self) -> MVMode { self.mvmode }
}

#[allow(dead_code)]
#[derive(Debug,Clone,Copy)]
pub struct PlusInfo {
    pub aic:        bool,
    pub deblock:    bool,
    pub aiv_mode:   bool,
    pub mq_mode:    bool,
}

impl PlusInfo {
    pub fn new(aic: bool, deblock: bool, aiv_mode: bool, mq_mode: bool) -> Self {
        PlusInfo { aic, deblock, aiv_mode, mq_mode }
    }
}

#[allow(dead_code)]
#[derive(Debug,Clone,Copy)]
pub struct SliceInfo {
    pub mb_x:   usize,
    pub mb_y:   usize,
    pub mb_end: usize,
    pub quant:  u8,
}

#[allow(dead_code)]
#[derive(Debug,Clone,Copy)]
pub struct SliceState {
    pub is_iframe:  bool,
    pub mb_x:       usize,
    pub mb_y:       usize,
    pub first_line: bool,
    pub first_mb:   bool,
    pub slice_mb_x: usize,
    pub slice_mb_y: usize,
    pub quant:      u8,
}

const SLICE_NO_END: usize = 99999999;

impl SliceInfo {
    pub fn new(mb_x: usize, mb_y: usize, mb_end: usize, quant: u8) -> Self {
        SliceInfo{ mb_x, mb_y, mb_end, quant }
    }
    pub fn new_gob(mb_x: usize, mb_y: usize, quant: u8) -> Self {
        SliceInfo{ mb_x, mb_y, mb_end: SLICE_NO_END, quant }
    }
    pub fn get_default_slice(pinfo: &PicInfo) -> Self {
        SliceInfo{ mb_x: 0, mb_y: 0, mb_end: SLICE_NO_END, quant: pinfo.get_quant() }
    }
    pub fn get_quant(&self) -> u8 { self.quant }
    pub fn is_at_end(&self, mb_pos: usize) -> bool { self.mb_end == mb_pos }
    pub fn needs_check(&self) -> bool { self.mb_end == SLICE_NO_END }
}

impl SliceState {
    pub fn new(is_iframe: bool) -> Self {
        SliceState {
            is_iframe, mb_x: 0, mb_y: 0, first_line: true, first_mb: true,
            slice_mb_x: 0, slice_mb_y: 0, quant: 0
        }
    }
    pub fn next_mb(&mut self) {
        self.mb_x += 1; self.first_mb = false;
        if self.mb_x >= self.slice_mb_x && self.mb_y > self.slice_mb_y {
            self.first_line = false;
        }
    }
    pub fn new_row(&mut self) {
        self.mb_x = 0; self.mb_y += 1;
        if self.mb_x >= self.slice_mb_x && self.mb_y > self.slice_mb_y {
            self.first_line = false;
        }
        self.first_mb = true;
    }
    pub fn reset_slice(&mut self, smb_x: usize, smb_y: usize) {
        self.slice_mb_x = smb_x;
        self.slice_mb_y = smb_y;
        self.first_line = true;
        self.first_mb   = true;
    }
}

#[derive(Debug,Clone,Copy)]
pub struct BlockInfo {
    pub intra:   bool,
    pub skip:    bool,
    pub mode:    Type,
    pub cbp:     u8,
    pub q:       u8,
    pub mv:      [MV; 4],
    pub num_mv:  usize,
    pub bpart:   bool,
    pub b_cbp:   u8,
    pub mv2:     [MV; 2],
    pub num_mv2: usize,
    pub fwd:     bool,
    pub acpred:  ACPredMode,
}

#[allow(dead_code)]
#[derive(Debug,Clone,Copy)]
pub struct BBlockInfo {
    present: bool,
    cbp:     u8,
    num_mv:  usize,
    fwd:     bool,
}

#[allow(non_camel_case_types)]
#[derive(Debug,Clone,Copy)]
pub enum BlockMVInfo {
    Intra,
    Inter_1MV(MV),
    Inter_4MV([MV; 4]),
}

#[allow(dead_code)]
#[derive(Debug,Clone,Copy,PartialEq)]
pub enum ACPredMode {
    None,
    DC,
    Ver,
    Hor,
}

#[allow(dead_code)]
impl BlockInfo {
    pub fn new(mode: Type, cbp: u8, q: u8) -> Self {
        BlockInfo {
            intra:   mode == Type::I,
            skip:    (cbp == 0) && (mode != Type::I),
            mode,
            cbp,
            q,
            mv:      [MV::new(0, 0), MV::new(0, 0), MV::new(0, 0), MV::new(0, 0)],
            num_mv:  0,
            bpart:   false,
            b_cbp:   0,
            mv2:     [ZERO_MV, ZERO_MV],
            num_mv2: 0,
            fwd:     false,
            acpred:  ACPredMode::None,
        }
    }
    pub fn is_intra(&self) -> bool { self.intra }
    pub fn is_skipped(&self) -> bool { self.skip }
    pub fn get_mode(&self) -> Type { self.mode }
    pub fn get_cbp(&self) -> u8 { self.cbp }
    pub fn get_q(&self) -> u8 { self.q }
    pub fn get_num_mvs(&self) -> usize { self.num_mv }
    pub fn get_mv(&self, idx: usize) -> MV { self.mv[idx] }
    pub fn has_b_part(&self) -> bool { self.bpart }
    pub fn get_cbp_b(&self) -> u8 { self.b_cbp }
    pub fn get_num_mvs2(&self) -> usize { self.num_mv2 }
    pub fn get_mv2(&self, idx: usize) -> MV { self.mv2[idx] }
    pub fn set_mv(&mut self, mvs: &[MV]) {
        if !mvs.is_empty() { self.skip = false; }
        let mut mv_arr: [MV; 4] = [MV::new(0, 0), MV::new(0, 0), MV::new(0, 0), MV::new(0, 0)];
        for i in 0..mvs.len() { mv_arr[i] = mvs[i]; }
        self.mv     = mv_arr;
        self.num_mv = mvs.len();
    }
    pub fn set_bpart(&mut self, bbinfo: BBlockInfo) {
        self.bpart = bbinfo.present;
        self.b_cbp = bbinfo.cbp;
        self.fwd   = bbinfo.fwd;
        self.num_mv2 = bbinfo.get_num_mv();
    }
    pub fn set_b_mv(&mut self, mvs: &[MV]) {
        if !mvs.is_empty() { self.skip = false; }
        let mut mv_arr: [MV; 2] = [ZERO_MV, ZERO_MV];
        for i in 0..mvs.len() { mv_arr[i] = mvs[i]; }
        self.mv2     = mv_arr;
        self.num_mv2 = mvs.len();
    }
    pub fn is_b_fwd(&self) -> bool { self.fwd }
    pub fn set_acpred(&mut self, acpred: ACPredMode) { self.acpred = acpred }
    pub fn get_acpred(&self) -> ACPredMode { self.acpred }
}

impl BBlockInfo {
    pub fn new(present: bool, cbp: u8, num_mv: usize, fwd: bool) -> Self {
        BBlockInfo {
            present,
            cbp,
            num_mv,
            fwd,
        }
    }
    pub fn get_num_mv(&self) -> usize { self.num_mv }
}

#[derive(Debug,Clone,Copy)]
pub enum MVMode {
    Old,
    Long,
    UMV,
}

pub trait H263MVTrait {
    fn add_umv(pred_mv: MV, add: MV, mvmode: MVMode) -> MV;
    fn scale(&self, trb: u16, trd: u16) -> MV;
    fn b_sub(pvec: MV, fwdvec: MV, bvec: MV, trb: u16, trd: u16) -> MV;
}

impl H263MVTrait for MV {
    fn add_umv(pred_mv: MV, add: MV, mvmode: MVMode) -> MV {
        let mut new_mv = pred_mv + add;
        match mvmode {
            MVMode::Old => {
                    if      new_mv.x >=  64 { new_mv.x -= 64; }
                    else if new_mv.x <= -64 { new_mv.x += 64; }
                    if      new_mv.y >=  64 { new_mv.y -= 64; }
                    else if new_mv.y <= -64 { new_mv.y += 64; }
                },
            MVMode::Long => {
                    if      new_mv.x >  31 { new_mv.x -= 64; }
                    else if new_mv.x < -32 { new_mv.x += 64; }
                    if      new_mv.y >  31 { new_mv.y -= 64; }
                    else if new_mv.y < -32 { new_mv.y += 64; }
                },
            MVMode::UMV => {
                    if pred_mv.x >  32 && new_mv.x >  63 { new_mv.x -= 64; }
                    if pred_mv.x < -31 && new_mv.x < -63 { new_mv.x += 64; }
                    if pred_mv.y >  32 && new_mv.y >  63 { new_mv.y -= 64; }
                    if pred_mv.y < -31 && new_mv.y < -63 { new_mv.y += 64; }
                },
        };
        new_mv
    }
    fn scale(&self, trb: u16, trd: u16) -> MV {
        if (trd == 0) || (trb == 0) {
            ZERO_MV
        } else {
            MV { x: (((self.x as i32) * (trb as i32)) / (trd as i32)) as i16, y: (((self.y as i32) * (trb as i32)) / (trd as i32)) as i16 }
        }
    }
    fn b_sub(pvec: MV, fwdvec: MV, bvec: MV, trb: u16, trd: u16) -> MV {
        let bscale = (trb as i32) - (trd as i32);
        let x = if bvec.x != 0 { fwdvec.x - pvec.x } else if trd != 0 { (bscale * (pvec.x as i32) / (trd as i32)) as i16 } else { 0 };
        let y = if bvec.y != 0 { fwdvec.y - pvec.y } else if trd != 0 { (bscale * (pvec.y as i32) / (trd as i32)) as i16 } else { 0 };
        MV { x, y }
    }
}

#[allow(dead_code)]
pub struct CBPInfo {
    cbp:        Vec<u8>,
    q:          Vec<u8>,
    mb_w:       usize,
}

impl CBPInfo {
    fn new() -> Self { CBPInfo{ cbp: Vec::new(), q: Vec::new(), mb_w: 0 } }
    fn reset(&mut self, mb_w: usize) {
        self.mb_w = mb_w;
        self.cbp.clear();
        self.cbp.resize(self.mb_w * 2, 0);
        self.q.clear();
        self.q.resize(self.mb_w * 2, 0);
    }
    fn update_row(&mut self) {
        for i in 0..self.mb_w {
            self.cbp[i] = self.cbp[self.mb_w + i];
            self.q[i]   = self.q[self.mb_w + i];
        }
    }
    fn set_cbp(&mut self, mb_x: usize, cbp: u8) {
        self.cbp[self.mb_w + mb_x] = cbp;
    }
    fn set_q(&mut self, mb_x: usize, q: u8) {
        self.q[self.mb_w + mb_x] = q;
    }
    pub fn get_q(&self, mb_x: usize) -> u8 { self.q[mb_x] }
    pub fn is_coded(&self, mb_x: usize, blk_no: usize) -> bool {
        (self.cbp[self.mb_w + mb_x] & (1 << (5 - blk_no))) != 0
    }
    pub fn is_coded_top(&self, mb_x: usize, blk_no: usize) -> bool {
        let cbp     = self.cbp[self.mb_w + mb_x];
        let cbp_top = self.cbp[mb_x];
        match blk_no {
            0 => { (cbp_top & 0b001000) != 0 },
            1 => { (cbp_top & 0b000100) != 0 },
            2 => { (cbp     & 0b100000) != 0 },
            3 => { (cbp     & 0b010000) != 0 },
            4 => { (cbp_top & 0b000010) != 0 },
            _ => { (cbp_top & 0b000001) != 0 },
        }
    }
}

