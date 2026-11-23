use super::{BlockTokens, MAX_DIST, RateControl, calc_diff};
use super::super::binkviddata::*;

const BINK_INV_SCAN: [usize; 64] = [
     0,  1,  4,  5,  8,  9, 12, 13,
     2,  3,  6,  7, 10, 11, 14, 15,
    24, 25, 44, 45, 16, 17, 20, 21,
    26, 27, 46, 47, 18, 19, 22, 23,
    28, 29, 32, 33, 48, 49, 52, 53,
    30, 31, 34, 35, 50, 51, 54, 55,
    36, 37, 40, 41, 56, 57, 60, 61,
    38, 39, 42, 43, 58, 59, 62, 63
];

trait WriteBit {
    fn write_bit(&mut self, val: u16);
    fn write_coef(&mut self, aval: u32, sign: bool, bits: u8);
}

impl WriteBit for BlockTokens {
    fn write_bit(&mut self, val: u16) {
        self.other.push((val, 1));
    }
    fn write_coef(&mut self, aval: u32, sign: bool, bits: u8) {
        if bits > 1 {
            self.other.push((aval as u16 & ((1 << (bits - 1)) - 1), bits - 1));
        }
        self.write_bit(sign as u16);
    }
}

pub struct DSP {
    diff:       [i16; 64],
    dct_i:      [u8; 64],
    dct_p:      [u8; 64],
    qmats:      QuantMats,
    i_start:    usize,
    i_len:      usize,
    p_start:    usize,
    p_len:      usize,
}

impl DSP {
    pub fn new() -> Self {
        let mut qmats = QuantMats::default();
        qmats.calc_binkb_quants();
        Self {
            diff:       [0; 64],
            dct_i:      [0; 64],
            dct_p:      [0; 64],
            qmats,
            i_start:    0,
            i_len:      16,
            p_start:    0,
            p_len:      16,
        }
    }
    pub fn get_diff(&mut self, mc_blk: &[u8; 64], cur_blk: &[u8; 64]) {
        for (dst, (&prev, &cur)) in self.diff.iter_mut()
                .zip(mc_blk.iter().zip(cur_blk.iter())) {
            *dst = i16::from(cur) - i16::from(prev);
        }
    }
    pub fn recon_residue(&self, dst: &mut [u8], dstride: usize, mc_blk: &[u8; 64]) {
        for (dline, (prow, drow)) in dst.chunks_mut(dstride)
                .zip(mc_blk.chunks_exact(8).zip(self.diff.chunks_exact(8))) {
            for (dst, (&prev, &diff)) in dline.iter_mut().zip(prow.iter().zip(drow.iter())) {
                *dst = (i16::from(prev) + diff) as u8;
            }
        }
    }
    pub fn recon_dct_i(&self, dst: &mut [u8], dstride: usize) {
        for (dline, srow) in dst.chunks_mut(dstride).zip(self.dct_i.chunks_exact(8)) {
            dline[..8].copy_from_slice(srow);
        }
    }
    pub fn recon_dct_p(&self, dst: &mut [u8], dstride: usize) {
        for (dline, srow) in dst.chunks_mut(dstride).zip(self.dct_p.chunks_exact(8)) {
            dline[..8].copy_from_slice(srow);
        }
    }

    pub fn try_residue(&self, tokens: &mut BlockTokens) -> u32 {
        let mut tree = Tree::new(true);
        let mut flat = [0; 64];
        let mut blen = [0; 64];

        for (&idx, &val) in BINK_INV_SCAN.iter().zip(self.diff.iter()) {
            flat[idx] = val;
            let aval = val.unsigned_abs();
            let mut b = 0u8;
            while (1 << b) <= aval {
                b += 1;
            }
            blen[idx] = b.saturating_sub(1);
        }

        let mut max_val = 0;
        let mut max_bits = 0;
        let mut bits = 0;
        let mut avals = [0; 64];
        let mut signs = [false; 64];
        for ((aval, sign), (&val, &vlen)) in avals.iter_mut().zip(signs.iter_mut())
                .zip(flat.iter().zip(blen.iter())) {
            *aval = val.unsigned_abs();
            *sign = val < 0;
            max_val  = max_val.max(*aval);
            max_bits = max_bits.max(vlen);
            bits += aval.count_ones();
        }

        if max_bits > 7 || bits > 127 {
            return MAX_DIST;
        }

        tokens.nresidues.push(bits as u8);
        tokens.other.push((max_bits as u16, 3));

        let mut nz_cand = Vec::with_capacity(64);
        let mut masks_left = bits + 1;
        'tree_loop: for cur_bits in (0..=max_bits).rev() {
            let mask = 1 << cur_bits;
            for &idx in nz_cand.iter() {
                tokens.write_bit(((avals[idx] & mask) != 0) as u16);
                if (avals[idx] & mask) != 0 {
                    masks_left -= 1;
                    if masks_left == 0 {
                        break 'tree_loop;
                    }
                }
            }

            let mut pos = tree.start;
            while pos < tree.end {
                if tree.state[pos] == TreeState::None {
                    pos += 1;
                    continue;
                }
                if let TreeState::Candidate(idx) = tree.state[pos] {
                    let idx = usize::from(idx);
                    if blen[idx] == cur_bits && flat[idx] != 0 {
                        tree.state[pos] = TreeState::None;
                        tokens.write_bit(1);
                        tokens.write_bit(signs[idx] as u16);
                        nz_cand.push(idx);
                        masks_left -= 1;
                        if masks_left == 0 {
                            break 'tree_loop;
                        }
                    } else {
                        tokens.write_bit(0);
                        pos += 1;
                    }
                    continue;
                }
                let range = tree.state[pos].get_range();
                let cur_max_bits = blen[range].iter().fold(0u8, |acc, &a| acc.max(a));
                if cur_max_bits == cur_bits {
                    tokens.write_bit(1);
                    match tree.state[pos] {
                        TreeState::Twenty(val) => {
                            tree.state[pos] = TreeState::Sixteen(val + 4);
                            for i in 0..4 {
                                let idx = usize::from(val) + i;
                                if blen[idx] == cur_bits && flat[idx] != 0 {
                                    tokens.write_bit(0);
                                    tokens.write_bit(signs[idx] as u16);
                                    nz_cand.push(idx);
                                    masks_left -= 1;
                                    if masks_left == 0 {
                                        break 'tree_loop;
                                    }
                                } else {
                                    tokens.write_bit(1);
                                    tree.add_to_head(TreeState::Candidate(idx as u8));
                                }
                            }
                        },
                        TreeState::Sixteen(val) => {
                            tree.state[pos] = TreeState::Four(val);
                            for i in 1u8..4 {
                                tree.add_to_tail(TreeState::Four(val + i * 4));
                            }
                        },
                        TreeState::Four(val) => {
                            tree.state[pos] = TreeState::None;
                            for i in 0..4 {
                                let idx = usize::from(val) + i;
                                if blen[idx] == cur_bits && flat[idx] != 0 {
                                    tokens.write_bit(0);
                                    tokens.write_bit(signs[idx] as u16);
                                    nz_cand.push(idx);
                                    masks_left -= 1;
                                    if masks_left == 0 {
                                        break 'tree_loop;
                                    }
                                } else {
                                    tokens.write_bit(1);
                                    tree.add_to_head(TreeState::Candidate(idx as u8));
                                }
                            }
                        },
                        _ => unreachable!(),
                    };
                } else {
                    tokens.write_bit(0);
                    pos += 1;
                }
            }
        }

        0
    }

    pub fn set_quant_ranges(&mut self, ranges: [u8; 4]) {
        self.i_start = usize::from(ranges[0]);
        self.i_len   = usize::from(ranges[1]);
        self.p_start = usize::from(ranges[2]);
        self.p_len   = usize::from(ranges[3]);
    }
    pub fn try_dct_intra(&mut self, blk: &[u8; 64], tokens: &mut BlockTokens, tmp_tok: &mut BlockTokens, is_b: bool, rc: &RateControl, mut best_dist: u32) -> u32 {
        tokens.clear();
        if self.i_len == 0 {
            return MAX_DIST;
        }

        let mut ref_coeffs = [0i32; 64];
        for (dst, &src) in ref_coeffs.iter_mut().zip(blk.iter()) {
            *dst = i32::from(src);
        }
        dct(&mut ref_coeffs);

        let mut dct_out = [0u8; 64];
        let qmats = if is_b { &self.qmats.intra_qmat } else { BINK_INTRA_QUANT };
        for (qidx, qmat) in qmats.iter().enumerate().skip(self.i_start).take(self.i_len) {
            let mut coeffs = ref_coeffs;
            for (idx, el) in coeffs.iter_mut().enumerate() {
                *el /= qmat[BINK_INV_SCAN[idx]];
            }

            if coeffs[0] >= 2048 {
                continue;
            }

            tmp_tok.clear();
            tmp_tok.intradc.push(coeffs[0] as u16);
            Self::code_dct_coeffs(&coeffs, tmp_tok);
            if is_b {
                tmp_tok.intraq.push(qidx as u8);
            } else {
                tmp_tok.other.push((qidx as u16, 4));
            }
            let bits = tmp_tok.bits(is_b);
            if rc.metric(0, bits) >= best_dist {
                continue;
            }

            for (idx, el) in coeffs.iter_mut().enumerate() {
                if *el != 0 {
                    *el = (*el * qmat[BINK_INV_SCAN[idx]]) >> 11;
                }
            }
            idct(&mut coeffs);
            for (dst, &src) in dct_out.iter_mut().zip(coeffs.iter()) {
                *dst = src as u8;
            }
            let diff = calc_diff(&dct_out, 8, blk, 8);
            let dist = rc.metric(diff, bits);
            if dist < best_dist {
                best_dist = dist;
                std::mem::swap(tokens, tmp_tok);
                self.dct_i.copy_from_slice(&dct_out);
            }
        }

        best_dist
    }
    #[allow(clippy::too_many_arguments)]
    pub fn try_dct_inter(&mut self, ref_blk: &[u8; 64], cur_blk: &[u8; 64], tokens: &mut BlockTokens, tmp_tok: &mut BlockTokens, is_b: bool, rc: &RateControl, mut best_dist: u32) -> u32 {
        let mv_x = tokens.xoff[0];
        let mv_y = tokens.yoff[0];

        let mut ref_coeffs = [0i32; 64];
        for (dst, &src) in ref_coeffs.iter_mut().zip(self.diff.iter()) {
            *dst = i32::from(src);
        }
        dct(&mut ref_coeffs);

        let mut dct_out = [0u8; 64];
        let qmats = if is_b { &self.qmats.inter_qmat } else { BINK_INTER_QUANT };
        for (qidx, qmat) in qmats.iter().enumerate().skip(self.p_start).take(self.p_len) {
            let mut coeffs = ref_coeffs;

            for (idx, el) in coeffs.iter_mut().enumerate() {
                *el /= qmat[BINK_INV_SCAN[idx]];
            }

            if coeffs[0].unsigned_abs() >= 1024 {
                continue;
            }

            tmp_tok.clear();
            tmp_tok.interdc.push(coeffs[0] as i16);
            tmp_tok.xoff.push(mv_x);
            tmp_tok.yoff.push(mv_y);
            Self::code_dct_coeffs(&coeffs, tmp_tok);
            if is_b {
                tmp_tok.interq.push(qidx as u8);
            } else {
                tmp_tok.other.push((qidx as u16, 4));
            }
            let bits = tmp_tok.bits(is_b);
            if rc.metric(0, bits) >= best_dist {
                continue;
            }

            for (idx, el) in coeffs.iter_mut().enumerate() {
                if *el != 0 {
                    *el = (*el * qmat[BINK_INV_SCAN[idx]]) >> 11;
                }
            }
            idct(&mut coeffs);
            for (dst, (&prev, &diff)) in dct_out.iter_mut().zip(ref_blk.iter().zip(coeffs.iter())) {
                *dst = (i32::from(prev) + diff) as u8;
            }
            let diff = calc_diff(&dct_out, 8, cur_blk, 8);
            let dist = rc.metric(diff, bits);
            if dist < best_dist {
                best_dist = dist;
                std::mem::swap(tokens, tmp_tok);
                self.dct_p.copy_from_slice(&dct_out);
            }
        }

        best_dist
    }

    fn code_dct_coeffs(coeffs: &[i32; 64], tokens: &mut BlockTokens) {
        let mut tree = Tree::new(false);
        let mut flat = [0; 64];
        let mut blen = [0; 64];

        for (&idx, &val) in BINK_INV_SCAN.iter().zip(coeffs.iter()).skip(1) {
            flat[idx] = val;
            let aval = val.unsigned_abs();
            let mut b = 0u8;
            while (1 << b) <= aval {
                b += 1;
            }
            blen[idx] = b;
        }

        let mut max_val = 0;
        let mut max_bits = 0;
        let mut avals = [0; 64];
        let mut signs = [false; 64];
        for ((aval, sign), (&val, &vlen)) in avals.iter_mut().zip(signs.iter_mut())
                .zip(flat.iter().zip(blen.iter())) {
            *aval = val.unsigned_abs();
            *sign = val < 0;
            max_val  = max_val.max(*aval);
            max_bits = max_bits.max(vlen);
        }

        tokens.other.push((u16::from(max_bits), 4));
        for cur_bits in (1..=max_bits).rev() {
            let mut pos = tree.start;
            while pos < tree.end {
                if tree.state[pos] == TreeState::None {
                    pos += 1;
                    continue;
                }
                if let TreeState::Candidate(idx) = tree.state[pos] {
                    let idx = usize::from(idx);
                    if blen[idx] == cur_bits && flat[idx] != 0 {
                        tree.state[pos] = TreeState::None;
                        tokens.write_bit(1);
                        tokens.write_coef(avals[idx], signs[idx], cur_bits);
                    } else {
                        tokens.write_bit(0);
                        pos += 1;
                    }
                    continue;
                }
                let range = tree.state[pos].get_range();
                let cur_max_bits = blen[range].iter().fold(0u8, |acc, &a| acc.max(a));
                if cur_max_bits == cur_bits {
                    tokens.write_bit(1);
                    match tree.state[pos] {
                        TreeState::Twenty(val) => {
                            tree.state[pos] = TreeState::Sixteen(val + 4);
                            for i in 0..4 {
                                let idx = usize::from(val) + i;
                                if blen[idx] == cur_bits && flat[idx] != 0 {
                                    tokens.write_bit(0);
                                    tokens.write_coef(avals[idx], signs[idx], cur_bits);
                                } else {
                                    tokens.write_bit(1);
                                    tree.add_to_head(TreeState::Candidate(idx as u8));
                                }
                            }
                        },
                        TreeState::Sixteen(val) => {
                            tree.state[pos] = TreeState::Four(val);
                            for i in 1u8..4 {
                                tree.add_to_tail(TreeState::Four(val + i * 4));
                            }
                        },
                        TreeState::Four(val) => {
                            tree.state[pos] = TreeState::None;
                            for i in 0..4 {
                                let idx = usize::from(val) + i;
                                if blen[idx] == cur_bits && flat[idx] != 0 {
                                    tokens.write_bit(0);
                                    tokens.write_coef(avals[idx], signs[idx], cur_bits);
                                } else {
                                    tokens.write_bit(1);
                                    tree.add_to_head(TreeState::Candidate(idx as u8));
                                }
                            }
                        },
                        _ => unreachable!(),
                    };
                } else {
                    tokens.write_bit(0);
                    pos += 1;
                }
            }
        }
    }
}

#[derive(Clone,Copy,Debug,PartialEq)]
enum TreeState {
    None,
    Twenty(u8),
    Sixteen(u8),
    Four(u8),
    Candidate(u8),
}

impl TreeState {
    fn get_range(self) -> std::ops::Range<usize> {
        let (base, len) = match self {
            TreeState::None => (0, 0),
            TreeState::Twenty(val) => (val, 20),
            TreeState::Sixteen(val) => (val, 16),
            TreeState::Four(val) => (val, 4),
            TreeState::Candidate(val) => (val, 1),
        };
        usize::from(base)..usize::from(base + len)
    }
}

struct Tree {
    state:  [TreeState; 128],
    start:  usize,
    end:    usize,
}

impl Tree {
    fn new(is_res: bool) -> Self {
        let mut state = [TreeState::None; 128];
        let start = 64;
        let mut end = start;

        state[end] = TreeState::Twenty(4);
        end += 1;
        state[end] = TreeState::Twenty(24);
        end += 1;
        state[end] = TreeState::Twenty(44);
        end += 1;
        if is_res {
            state[end] = TreeState::Four(0);
            end += 1;
        } else {
            for i in 1..4 {
                state[end] = TreeState::Candidate(i);
                end += 1;
            }
        }
        Self { state, start, end }
    }
    fn add_to_tail(&mut self, ts: TreeState) {
        self.state[self.end] = ts;
        self.end += 1;
    }
    fn add_to_head(&mut self, ts: TreeState) {
        self.start -= 1;
        self.state[self.start] = ts;
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

fn idct(coeffs: &mut [i32; 64]) {
    let mut tmp: [i32; 64] = [0; 64];
    let mut row: [i32; 8] = [0; 8];
    for i in 0..8 {
        idct!(coeffs, 8, tmp, 8, i, 0, 0);
    }
    for (drow, srow) in coeffs.chunks_exact_mut(8).zip(tmp.chunks_exact(8)) {
        idct!(srow, 1, row, 1, 0, 0x7F, 8);
        drow.copy_from_slice(&row);
    }
}

const B1: i32 = 2896;
const B2: i32 = 3789;
const B3: i32 = 1569;
const B4: i32 = 4464;
const B5: i32 = 6679;
const B6: i32 = 1327;
const B7: i32 =  888;
macro_rules! dct {
    ($src: expr, $sstep: expr, $dst: expr, $dstep: expr, $off: expr, $bias: expr, $shift: expr) => {
        let a0 = $src[$off + 0 * $sstep] + $src[$off + 7 * $sstep];
        let a1 = $src[$off + 0 * $sstep] - $src[$off + 7 * $sstep];
        let a2 = $src[$off + 1 * $sstep] + $src[$off + 6 * $sstep];
        let a3 = $src[$off + 1 * $sstep] - $src[$off + 6 * $sstep];
        let a4 = $src[$off + 2 * $sstep] + $src[$off + 5 * $sstep];
        let a5 = $src[$off + 2 * $sstep] - $src[$off + 5 * $sstep];
        let a6 = $src[$off + 3 * $sstep] + $src[$off + 4 * $sstep];
        let a7 = $src[$off + 3 * $sstep] - $src[$off + 4 * $sstep];

        let b0 = (a0 + a4) << 7;
        let b1 = (a0 - a4) << 7;
        let b2 = (a2 + a6) << 7;
        let b3 = (a2 - a6) << 7;

        $dst[$off + 0 * $dstep] = (b0 + b2 + $bias) >> $shift;
        $dst[$off + 4 * $dstep] = (b1 - b3 + $bias) >> $shift;

        let c0 = (a0 - a6) << 7;
        let c1 = B1.wrapping_mul((b1 + b3) >> 7) >> 5;
        $dst[$off + 2 * $dstep] = (c0 + c1 + $bias) >> $shift;
        $dst[$off + 6 * $dstep] = (c0 - c1 + $bias) >> $shift;

        let d0 = B2.wrapping_mul(a1) + B3.wrapping_mul(a7);
        let d1 = ( d0 + B4.wrapping_mul(a5) + B5.wrapping_mul(a3) + (1 << 4)) >> 5;
        let d2 = (-d0 + B6.wrapping_mul(a5) - B7.wrapping_mul(a3) + (1 << 4)) >> 5;
        $dst[$off + 1 * $dstep] = ((a1 << 7) + d1 + $bias) >> $shift;
        $dst[$off + 7 * $dstep] = ((a1 << 7) + d2 + $bias) >> $shift;

        let e0 = B3.wrapping_mul(a1) - B2.wrapping_mul(a7);
        let e1 = ( e0 - B6.wrapping_mul(a3) - B5.wrapping_mul(a5) + (1 << 4)) >> 5;
        let e2 = (-e0 - B4.wrapping_mul(a3) + B7.wrapping_mul(a5) + (1 << 4)) >> 5;

        $dst[$off + 3 * $dstep] = ((a1 << 7) + e1 + $bias) >> $shift;
        $dst[$off + 5 * $dstep] = ((a1 << 7) + e2 + $bias) >> $shift;
    };
}

fn dct(coeffs: &mut [i32; 64]) {
    let mut tmp: [i32; 64] = [0; 64];
    let mut row: [i32; 8] = [0; 8];
    for i in 0..8 {
        dct!(coeffs, 8, tmp, 8, i, 1, 1);
    }
    for (drow, srow) in coeffs.chunks_exact_mut(8).zip(tmp.chunks_exact(8)) {
        dct!(srow, 1, row, 1, 0, 0, 0);
        drow.copy_from_slice(&row);
    }
}

