use std::mem;
use nihav_core::codecs::*;
use nihav_core::io::byteio::*;
use nihav_core::io::bitreader::*;
use nihav_core::io::codebook::*;
use nihav_core::io::intcode::*;
use nihav_codec_support::codecs::{IPShuffler, MV, ZERO_MV};

macro_rules! mktag {
    ($a:expr, $b:expr, $c:expr, $d:expr) => ({
        (($a as u32) << 24) | (($b as u32) << 16) | (($c as u32) << 8) | ($d as u32)
    });
    ($arr:expr) => ({
        (($arr[0] as u32) << 24) | (($arr[1] as u32) << 16) | (($arr[2] as u32) << 8) | ($arr[3] as u32)
    });
}

macro_rules! idct_mul {
    (a; $val: expr) => ($val + ($val >> 2));
    (b; $val: expr) => ($val >> 1);
    (c; $val: expr) => ($val - ($val >> 2) - ($val >> 4));
    (d; $val: expr) => ($val + ($val >> 2) - ($val >> 4));
    (e; $val: expr) => ($val >> 2);
}

macro_rules! idct {
    ($src: expr, $sstep: expr, $off: expr, $dst: expr, $dstep: expr, $doff: expr, $bias: expr, $shift: expr) => {
        let tmp00 = $src[$off + 3 * $sstep] + $src[$off + 5 * $sstep];
        let tmp01 = $src[$off + 3 * $sstep] - $src[$off + 5 * $sstep];
        let tmp02 = idct_mul!(a; $src[$off + 2 * $sstep]) + idct_mul!(b; $src[$off + 6 * $sstep]);
        let tmp03 = idct_mul!(b; $src[$off + 2 * $sstep]) - idct_mul!(a; $src[$off + 6 * $sstep]);
        let tmp0 = ($src[$off + 0 * $sstep] + $src[$off + 4 * $sstep]) + tmp02;
        let tmp1 = ($src[$off + 0 * $sstep] + $src[$off + 4 * $sstep]) - tmp02;
        let tmp2 = $src[$off + 0 * $sstep] - $src[$off + 4 * $sstep];
        let tmp3 = $src[$off + 1 * $sstep] + tmp00;
        let tmp4 = $src[$off + 1 * $sstep] - tmp00;
        let tmp5 = tmp01 + $src[$off + 7 * $sstep];
        let tmp6 = tmp01 - $src[$off + 7 * $sstep];
        let tmp7 = tmp4 + idct_mul!(c; tmp6);
        let tmp8 = idct_mul!(c; tmp4) - tmp6;
        let tmp9  = idct_mul!(d; tmp3) + idct_mul!(e; tmp5);
        let tmp10 = idct_mul!(e; tmp3) - idct_mul!(d; tmp5);
        let tmp11 = tmp2 + tmp03;
        let tmp12 = tmp2 - tmp03;

        $dst[$doff + 0 * $dstep] = (tmp0  + tmp9  + $bias) >> $shift;
        $dst[$doff + 1 * $dstep] = (tmp11 + tmp7  + $bias) >> $shift;
        $dst[$doff + 2 * $dstep] = (tmp12 + tmp8  + $bias) >> $shift;
        $dst[$doff + 3 * $dstep] = (tmp1  + tmp10 + $bias) >> $shift;
        $dst[$doff + 4 * $dstep] = (tmp1  - tmp10 + $bias) >> $shift;
        $dst[$doff + 5 * $dstep] = (tmp12 - tmp8  + $bias) >> $shift;
        $dst[$doff + 6 * $dstep] = (tmp11 - tmp7  + $bias) >> $shift;
        $dst[$doff + 7 * $dstep] = (tmp0  - tmp9  + $bias) >> $shift;
    };
    (float; $src: expr, $sstep: expr, $off: expr, $dst: expr, $dstep: expr, $doff: expr, $bias: expr, $shift: expr) => {
        let t00 =  $src[$off + $sstep * 2] + $src[$off + $sstep * 6];
        let t01 = ($src[$off + $sstep * 2] - $src[$off + $sstep * 6]) * std::f32::consts::SQRT_2 - t00;
        let t02 = $src[$off + $sstep * 0] + $src[$off + $sstep * 4];
        let t03 = $src[$off + $sstep * 0] - $src[$off + $sstep * 4];
        let t04 = $src[$off + $sstep * 3] + $src[$off + $sstep * 5];
        let t05 = $src[$off + $sstep * 3] - $src[$off + $sstep * 5];
        let t06 = $src[$off + $sstep * 1] + $src[$off + $sstep * 7];
        let t07 = $src[$off + $sstep * 1] - $src[$off + $sstep * 7];
        let t08 = t02 + t00;
        let t09 = t02 - t00;
        let t10 = t03 + t01;
        let t11 = t03 - t01;
        let t12 = t06 + t04;
        let t13 = (t06 - t04) * std::f32::consts::SQRT_2;
        let t14 = (t07 - t05) * 1.847759;
        let t15 = t05 * 2.613126 + t14 - t12;
        let t16 = t13 - t15;
        let t17 = t07 * 1.0823922 - t14 + t16;

        $dst[$doff + 0 * $dstep] = t08 + t12;
        $dst[$doff + 1 * $dstep] = t10 + t15;
        $dst[$doff + 2 * $dstep] = t11 + t16;
        $dst[$doff + 3 * $dstep] = t09 - t17;
        $dst[$doff + 4 * $dstep] = t09 + t17;
        $dst[$doff + 5 * $dstep] = t11 - t16;
        $dst[$doff + 6 * $dstep] = t10 - t15;
        $dst[$doff + 7 * $dstep] = t08 - t12;
    };
}

#[allow(clippy::erasing_op)]
fn bink2_idct(coeffs: &mut [i32; 64]) {
    let mut tmp: [i32; 64] = [0; 64];
    for i in 0..8 {
        idct!(coeffs, 1, 8 * i, tmp, 8, i, 0, 0);
    }
    for i in 0..8 {
        idct!(tmp, 1, 8 * i, coeffs, 1, 8 * i, 0, 6);
    }
}

#[allow(clippy::erasing_op)]
fn bink2_idct_old(coeffs: &mut [f32; 64]) {
    let mut tmp: [f32; 64] = [0.0; 64];
    coeffs[0] += 512.5;
    for i in 0..8 {
        idct!(float; coeffs, 8, i, tmp, 8, i, 0, 0);
    }
    for i in 0..8 {
        idct!(float; tmp, 1, 8 * i, coeffs, 1, 8 * i, 0, 6);
    }
}

struct Bink2DSP { }

fn clip8(val: i32) -> u8 {
    val.clamp(0, 255) as u8
}

macro_rules! el {
    ($src: expr, $off: expr) => (
            $src[$off] as i32
        );
}

macro_rules! luma_filter {
    ($src: expr, $off: expr, $step: expr) => ({
            let t0 = el!($src, $off - 0 * $step) + el!($src, $off + 1 * $step);
            let t1 = el!($src, $off - 1 * $step) + el!($src, $off + 2 * $step);
            let t2 = el!($src, $off - 2 * $step) + el!($src, $off + 3 * $step);
            (((t0 * 19) >> 1) - t1 * 2 + (t2 >> 1) + 8) >> 4
        });
}

macro_rules! chroma_interp {
    ($dst: expr, $dstride: expr, $h: expr, $ty: tt, $src: expr, $sstride: expr, $step: expr, $mode: expr, $shift: expr, $add: expr) => {
        let mut soff = 0;
        for out in $dst.chunks_mut($dstride).take($h) {
            for i in 0..8 {
                let e0 = el!($src, soff + i);
                let e1 = el!($src, soff + i + $step);
                out[i] = match $mode {
                        0 => e0,
                        1 => (e0 * 3 + e1     + $add) >> $shift,
                        2 => (e0 * 2 + e1 * 2 + $add) >> $shift,
                        _ => (e0     + e1 * 3 + $add) >> $shift,
                    } as $ty;
            }
            soff += $sstride;
        }
    };
}

macro_rules! avg_tree {
    ($a: expr, $b: expr) => (($a + $b + 1) >> 1);
    ($a: expr, $b: expr, $c: expr, $d: expr) => (avg_tree!(avg_tree!($a, $b), avg_tree!($c, $d)));
}

#[allow(clippy::erasing_op)]
impl Bink2DSP {
    #[allow(clippy::identity_op)]
    fn calc_dc(src: &[u8], stride: usize) -> i32 {
        let mut sums = [0u16; 8];
        for i in 0..8 {
            let s0 = src[i + stride * 0] as u16;
            let s1 = src[i + stride * 1] as u16;
            let s2 = src[i + stride * 2] as u16;
            let s3 = src[i + stride * 3] as u16;
            let s4 = src[i + stride * 4] as u16;
            let s5 = src[i + stride * 5] as u16;
            let s6 = src[i + stride * 6] as u16;
            let s7 = src[i + stride * 7] as u16;
            sums[i] = avg_tree!(avg_tree!(s0, s1, s2, s3), avg_tree!(s4, s5, s6, s7));
        }
        let mut sum = 0;
        for e in sums.iter() {
            sum += e;
        }
        sum as i32
    }
    fn put_mb4(dst: &mut [u8], mut off: usize, stride: usize, blk: &mut [[i32; 64]; 4]) {
        bink2_idct(&mut blk[0]);
        bink2_idct(&mut blk[1]);
        bink2_idct(&mut blk[2]);
        bink2_idct(&mut blk[3]);
        {
            let dout = &mut dst[off..];
            for (row, (b0, b1)) in dout.chunks_mut(stride).zip(blk[0].chunks(8).zip(blk[1].chunks(8))) {
                for (dst, &c) in row[..8].iter_mut().zip(b0.iter()) {
                    *dst = clip8(c);
                }
                for (dst, &c) in row[8..16].iter_mut().zip(b1.iter()) {
                    *dst = clip8(c);
                }
            }
        }
        off += stride * 8;
        {
            let dout = &mut dst[off..];
            for (row, (b2, b3)) in dout.chunks_mut(stride).zip(blk[2].chunks(8).zip(blk[3].chunks(8))) {
                for (dst, &c) in row[..8].iter_mut().zip(b2.iter()) {
                    *dst = clip8(c);
                }
                for (dst, &c) in row[8..16].iter_mut().zip(b3.iter()) {
                    *dst = clip8(c);
                }
            }
        }
    }
    fn add_mb4(dst: &mut [u8], mut off: usize, stride: usize, blk: &mut [[i32; 64]; 4]) {
        bink2_idct(&mut blk[0]);
        bink2_idct(&mut blk[1]);
        bink2_idct(&mut blk[2]);
        bink2_idct(&mut blk[3]);
        {
            let dout = &mut dst[off..];
            for (row, (b0, b1)) in dout.chunks_mut(stride).zip(blk[0].chunks(8).zip(blk[1].chunks(8))) {
                for (dst, &add) in row[..8].iter_mut().zip(b0.iter()) {
                    *dst = clip8(i32::from(*dst) + add);
                }
                for (dst, &add) in row[8..16].iter_mut().zip(b1.iter()) {
                    *dst = clip8(i32::from(*dst) + add);
                }
            }
        }
        off += stride * 8;
        {
            let dout = &mut dst[off..];
            for (row, (b2, b3)) in dout.chunks_mut(stride).zip(blk[2].chunks(8).zip(blk[3].chunks(8))) {
                for (dst, &add) in row[..8].iter_mut().zip(b2.iter()) {
                    *dst = clip8(i32::from(*dst) + add);
                }
                for (dst, &add) in row[8..16].iter_mut().zip(b3.iter()) {
                    *dst = clip8(i32::from(*dst) + add);
                }
            }
        }
    }
    fn put_mb4_old(dst: &mut [u8], mut off: usize, stride: usize, blk: &mut [[f32; 64]; 4]) {
        bink2_idct_old(&mut blk[0]);
        bink2_idct_old(&mut blk[1]);
        bink2_idct_old(&mut blk[2]);
        bink2_idct_old(&mut blk[3]);
        {
            let dout = &mut dst[off..];
            for (row, (b0, b1)) in dout.chunks_mut(stride).zip(blk[0].chunks(8).zip(blk[1].chunks(8))) {
                for (dst, &c) in row[..8].iter_mut().zip(b0.iter()) {
                    *dst = clip8((c as i32) - 512);
                }
                for (dst, &c) in row[8..16].iter_mut().zip(b1.iter()) {
                    *dst = clip8((c as i32) - 512);
                }
            }
        }
        off += stride * 8;
        {
            let dout = &mut dst[off..];
            for (row, (b2, b3)) in dout.chunks_mut(stride).zip(blk[2].chunks(8).zip(blk[3].chunks(8))) {
                for (dst, &c) in row[..8].iter_mut().zip(b2.iter()) {
                    *dst = clip8((c as i32) - 512);
                }
                for (dst, &c) in row[8..16].iter_mut().zip(b3.iter()) {
                    *dst = clip8((c as i32) - 512);
                }
            }
        }
    }
    fn add_mb4_old(dst: &mut [u8], mut off: usize, stride: usize, blk: &mut [[f32; 64]; 4]) {
        bink2_idct_old(&mut blk[0]);
        bink2_idct_old(&mut blk[1]);
        bink2_idct_old(&mut blk[2]);
        bink2_idct_old(&mut blk[3]);
        {
            let dout = &mut dst[off..];
            for (row, (b0, b1)) in dout.chunks_mut(stride).zip(blk[0].chunks(8).zip(blk[1].chunks(8))) {
                for (dst, &add) in row[..8].iter_mut().zip(b0.iter()) {
                    *dst = clip8(i32::from(*dst) + (add as i32) - 512);
                }
                for (dst, &add) in row[8..16].iter_mut().zip(b1.iter()) {
                    *dst = clip8(i32::from(*dst) + (add as i32) - 512);
                }
            }
        }
        off += stride * 8;
        {
            let dout = &mut dst[off..];
            for (row, (b2, b3)) in dout.chunks_mut(stride).zip(blk[2].chunks(8).zip(blk[3].chunks(8))) {
                for (dst, &add) in row[..8].iter_mut().zip(b2.iter()) {
                    *dst = clip8(i32::from(*dst) + (add as i32) - 512);
                }
                for (dst, &add) in row[8..16].iter_mut().zip(b3.iter()) {
                    *dst = clip8(i32::from(*dst) + (add as i32) - 512);
                }
            }
        }
    }
    fn mc_luma(dst: &mut [u8], stride: usize, ref_pic: &NAVideoBuffer<u8>, xoff: usize, yoff: usize, mv: MV, plane: usize) -> DecoderResult<()> {
        let sx = (xoff as isize) + ((mv.x >> 1) as isize);
        let sy = (yoff as isize) + ((mv.y >> 1) as isize);
        let mode = (mv.x & 1) + (mv.y & 1) * 2;
        let (d_x, add_x) = if (mv.x & 1) != 0 { (2, 5) } else { (0, 0) };
        let (d_y, add_y) = if (mv.y & 1) != 0 { (2, 5) } else { (0, 0) };

        let (w, h) = ref_pic.get_dimensions(plane);
        let align_w = ((w + 31) & !31) as isize;
        let align_h = ((h + 31) & !31) as isize;
        validate!((sx - d_x >= 0) && (sx - d_x + add_x + 16 <= align_w));
        validate!((sy - d_y >= 0) && (sy - d_y + add_y + 16 <= align_h));
        let pstride = ref_pic.get_stride(plane);
        let mut poff = ref_pic.get_offset(plane) + (sx as usize) + (sy as usize) * pstride;
        let pdata = ref_pic.get_data();
        let ppix = pdata.as_slice();

        let dst = &mut dst[xoff + (yoff & 31) * stride..];

        match mode {
            0 => {
                let src = &ppix[poff..];
                for (out, row) in dst.chunks_mut(stride).take(16).zip(src.chunks(pstride)) {
                    out[..16].copy_from_slice(&row[..16]);
                }
            },
            1 => {
                for out in dst.chunks_mut(stride).take(16) {
                    for (i, el) in out[..16].iter_mut().enumerate() {
                        *el = clip8(luma_filter!(ppix, poff + i, 1));
                    }
                    poff += pstride;
                }
            },
            2 => {
                for out in dst.chunks_mut(stride).take(16) {
                    for i in 0..16 {
                        out[i] = clip8(luma_filter!(ppix, poff + i, pstride));
                    }
                    poff += pstride;
                }
            },
            3 => {
                let mut tmp = [0i16; 21 * 16];
                for out in tmp.chunks_mut(16) {
                    for i in 0..16 {
                        out[i] = luma_filter!(ppix, poff - 2 * pstride + i, 1) as i16;
                    }
                    poff += pstride;
                }
                for (row, out) in dst.chunks_mut(stride).take(16).enumerate() {
                    for i in 0..16 {
                        out[i] = clip8(luma_filter!(tmp, (row + 2) * 16 + i, 16));
                    }
                }
            },
            _ => unreachable!(),
        };
        Ok(())
    }
    // kb2i_round: KB2i rounds the second (vertical) pass of the 2D chroma interpolation to nearest (+8 of 16);
    // the older versions (and the original NihAV code) add only 4, which biases chroma low by 1/4 per moving block.
    fn mc_chroma(dst: &mut [u8], stride: usize, ref_pic: &NAVideoBuffer<u8>, xoff: usize, yoff: usize, mv: MV, plane: usize, kb2i_round: bool) -> DecoderResult<()> {
        let sx = (xoff as isize) + ((mv.x >> 2) as isize);
        let sy = (yoff as isize) + ((mv.y >> 2) as isize);
        let mx = mv.x & 3;
        let my = mv.y & 3;
        let add_x = if mx != 0 { 1 } else { 0 };
        let add_y = if my != 0 { 1 } else { 0 };

        let (w, h) = ref_pic.get_dimensions(plane);
        let align_w = ((w + 15) & !15) as isize;
        let align_h = ((h + 15) & !15) as isize;
        validate!((sx >= 0) && (sx + add_x + 8 <= align_w));
        validate!((sy >= 0) && (sy + add_y + 8 <= align_h));
        let pstride = ref_pic.get_stride(plane);
        let poff = ref_pic.get_offset(plane) + (sx as usize) + (sy as usize) * pstride;
        let pdata = ref_pic.get_data();
        let ppix = pdata.as_slice();

        let dst = &mut dst[xoff + (yoff & 15) * stride..];
        if (mx == 0) && (my == 0) {
            let inpix = &ppix[poff..];
            for (out, src) in dst.chunks_mut(stride).take(8).zip(inpix.chunks(pstride)) {
                out[..8].copy_from_slice(&src[..8]);
            }
        } else if my == 0 {
            chroma_interp!(dst, stride, 8, u8, &ppix[poff..], pstride, 1, mx, 2, 2);
        } else if mx == 0 {
            chroma_interp!(dst, stride, 8, u8, &ppix[poff..], pstride, pstride, my, 2, 2);
        } else {
            let mut tmp = [0u16; 9 * 8];
            chroma_interp!(tmp, 8, 9, u16, &ppix[poff..], pstride, 1, mx, 0, 0);
            chroma_interp!(dst, stride, 8, u8, &tmp, 8, 8, my, 4, if kb2i_round { 8 } else { 4 });
        }
        Ok(())
    }
}

#[allow(clippy::collapsible_else_if)]
fn mid_pred<T:PartialOrd>(a: T, b: T, c: T) -> T {
    if a < b {
        if b < c {
            b
        } else {
            if a < c { c } else { a }
        }
    } else {
        if b < c {
            if a < c { a } else { c }
        } else {
            b
        }
    }
}

#[derive(Default)]
struct QuantInfo {
    quants:     Vec<u8>,
    qstride:    usize,
    qpos:       usize,
    pqpos:      usize,
}

impl QuantInfo {
    fn resize(&mut self, bw: usize) {
        self.qstride = bw * 2;
        self.quants.resize(self.qstride * 2, 0);
        self.qpos = 0;
        self.pqpos = self.qstride;
    }
    fn update_line(&mut self) {
        mem::swap(&mut self.qpos, &mut self.pqpos);
    }
    fn pred_quant(&self, pos: usize, is_top: bool) -> u8 {
        let is_left = pos < 2;
        if is_top {
            if is_left { 16 } else { self.quants[self.qpos + pos - 2] }
        } else if is_left {
            self.quants[self.pqpos + pos]
        } else {
            mid_pred(self.quants[self.pqpos + pos - 2],
                     self.quants[self.pqpos + pos],
                     self.quants[self.qpos + pos - 2])
        }
    }
    fn set_quant(&mut self, pos: usize, q: u8) {
        self.quants[self.qpos + pos] = q;
    }
}

#[derive(Default)]
struct YDCInfo {
    dcs:        [i32; 16],
    dc_buf:     [i32; 16],
    prev_dcs:   Vec<i32>,
    new_dcs:    Vec<i32>,
    stride:     usize,
    prev_off:   usize,
}

impl YDCInfo {
    fn resize(&mut self, bw: usize) {
        self.stride     = bw * 4 + 4;
        self.prev_off   = 4;
        self.prev_dcs.resize(self.stride, 0);
        self.new_dcs.resize(self.stride, 0);
    }
    fn update_line(&mut self) {
        mem::swap(&mut self.prev_dcs, &mut self.new_dcs);
    }
    fn set_dcs(&mut self, bx: usize, dc5: i32, dc7: i32, dc13: i32, dc10: i32, dc11: i32, dc14: i32, dc15: i32) {
        self.dcs[ 5] = dc5;
        self.dcs[ 7] = dc7;
        self.dcs[13] = dc13;
        self.dcs[15] = dc15;
        let new_dc = &mut self.new_dcs[bx * 4 + 4..];
        new_dc[0] = dc10;
        new_dc[1] = dc11;
        new_dc[2] = dc14;
        new_dc[3] = dc15;
    }
}

#[derive(Default)]
struct CDCInfo {
    dcs:        [i32; 4],
    dc_buf:     [i32; 4],
    prev_dcs:   Vec<i32>,
    new_dcs:    Vec<i32>,
    stride:     usize,
    prev_off:   usize,
}

impl CDCInfo {
    fn resize(&mut self, bw: usize) {
        self.stride     = bw * 2 + 2;
        self.prev_off   = 2;
        self.prev_dcs.resize(self.stride, 0);
        self.new_dcs.resize(self.stride, 0);
    }
    fn update_line(&mut self) {
        mem::swap(&mut self.prev_dcs, &mut self.new_dcs);
    }
    fn set_dcs(&mut self, bx: usize, dc1: i32, dc2: i32, dc3: i32) {
        self.dcs[1] = dc1;
        self.dcs[2] = dc2;
        self.dcs[3] = dc3;
        let new_dc = &mut self.new_dcs[bx * 2 + 2..];
        new_dc[0] = dc2;
        new_dc[1] = dc3;
    }
}

trait DCInfo {
    fn decode_dcs(&mut self, br: &mut BitReader, q: u8) -> DecoderResult<bool>;
    fn predict(&mut self, bx: usize, is_top: bool, is_left: bool, min_dc: i32, max_dc: i32);
    fn predict_inter(&mut self, min_dc: i32, max_dc: i32);
}

fn dc_pred2(a: i32, b: i32) -> i32 {
    (a.max(b)).min(a.min(b).max(a * 2 - b))
}
fn dc_pred(a: i32, b: i32, c: i32) -> i32 {
    (a.max(b).max(c)).min((a.min(b).min(c)).max(b - a + c))
}

#[allow(clippy::identity_op)]
impl DCInfo for YDCInfo {
    fn decode_dcs(&mut self, br: &mut BitReader, q: u8) -> DecoderResult<bool> {
        decode_dcs(br, &mut self.dc_buf, q)
    }
    #[allow(non_snake_case)]
    #[allow(clippy::collapsible_else_if)]
    #[allow(clippy::needless_late_init)]
    fn predict(&mut self, bx: usize, is_top: bool, is_left: bool, min_dc: i32, max_dc: i32) {
        let a0; let a1; let a2; let a3; let a4; let a5; let a6; let a7;
        let a8; let a9; let aA; let aB; let aC; let aD; let aE; let aF;
        let prev_dc = &self.prev_dcs[bx * 4..];
        let dst = &mut self.dcs;
        let dcs = &self.dc_buf;
        if is_left {
            if is_top { //zigzag in 2x2 blocks
                let add = 1 << 10;//if has_prev_dc { 1 << 10 } else { 0 };
                a0 = (dcs[ 0] + add).max(min_dc).min(max_dc);
                a1 = (dcs[ 1] + a0).max(min_dc).min(max_dc);
                a2 = (dcs[ 2] + dc_pred2(a0, a1)).max(min_dc).min(max_dc);
                a3 = (dcs[ 3] + dc_pred(a0, a1, a2)).max(min_dc).min(max_dc);

                a4 = (dcs[ 4] + dc_pred2(a1, a3)).max(min_dc).min(max_dc);
                a5 = (dcs[ 5] + a4).max(min_dc).min(max_dc);
                a6 = (dcs[ 6] + dc_pred(a1, a3, a4)).max(min_dc).min(max_dc);
                a7 = (dcs[ 7] + dc_pred(a4, a5, a6)).max(min_dc).min(max_dc);

                a8 = (dcs[ 8] + dc_pred2(a2, a3)).max(min_dc).min(max_dc);
                a9 = (dcs[ 9] + dc_pred(a2, a3, a8)).max(min_dc).min(max_dc);
                aA = (dcs[10] + dc_pred2(a8, a9)).max(min_dc).min(max_dc);
                aB = (dcs[11] + dc_pred(a8, a9, aA)).max(min_dc).min(max_dc);

                aC = (dcs[12] + dc_pred(a3, a6, a9)).max(min_dc).min(max_dc);
                aD = (dcs[13] + dc_pred(a6, a7, aC)).max(min_dc).min(max_dc);
                aE = (dcs[14] + dc_pred(a9, aB, aC)).max(min_dc).min(max_dc);
                aF = (dcs[15] + dc_pred(aC, aD, aE)).max(min_dc).min(max_dc);
            } else {
                a0 = (dcs[ 0] + dc_pred2(prev_dc[4], prev_dc[5])).max(min_dc).min(max_dc);
                a1 = (dcs[ 1] + dc_pred(prev_dc[4], prev_dc[5], a0)).max(min_dc).min(max_dc);
                a2 = (dcs[ 2] + dc_pred2(a0, a1)).max(min_dc).min(max_dc);
                a3 = (dcs[ 3] + dc_pred(a0, a1, a2)).max(min_dc).min(max_dc);

                a4 = (dcs[ 4] + dc_pred(prev_dc[5], prev_dc[6], a1)).max(min_dc).min(max_dc);
                a5 = (dcs[ 5] + dc_pred(prev_dc[6], prev_dc[7], a4)).max(min_dc).min(max_dc);
                a6 = (dcs[ 6] + dc_pred(a1, a3, a4)).max(min_dc).min(max_dc);
                a7 = (dcs[ 7] + dc_pred(a4, a5, a6)).max(min_dc).min(max_dc);

                a8 = (dcs[ 8] + dc_pred2(a2, a3)).max(min_dc).min(max_dc);
                a9 = (dcs[ 9] + dc_pred(a2, a3, a8)).max(min_dc).min(max_dc);
                aA = (dcs[10] + dc_pred2(a8, a9)).max(min_dc).min(max_dc);
                aB = (dcs[11] + dc_pred(a8, a9, aA)).max(min_dc).min(max_dc);

                aC = (dcs[12] + dc_pred(a3, a6, a9)).max(min_dc).min(max_dc);
                aD = (dcs[13] + dc_pred(a6, a7, aC)).max(min_dc).min(max_dc);
                aE = (dcs[14] + dc_pred(a9, aB, aC)).max(min_dc).min(max_dc);
                aF = (dcs[15] + dc_pred(aC, aD, aE)).max(min_dc).min(max_dc);
            }
        } else {
            if is_top {
                a0 = (dcs[ 0] + dc_pred2(dst[5], dst[7])).max(min_dc).min(max_dc);
                a1 = (dcs[ 1] + a0).max(min_dc).min(max_dc);
                a2 = (dcs[ 2] + dc_pred(dst[5], dst[7], a0)).max(min_dc).min(max_dc);
                a3 = (dcs[ 3] + dc_pred(a0, a1, a2)).max(min_dc).min(max_dc);

                a4 = (dcs[ 4] + dc_pred2(a1, a3)).max(min_dc).min(max_dc);
                a5 = (dcs[ 5] + a4).max(min_dc).min(max_dc);
                a6 = (dcs[ 6] + dc_pred(a1, a3, a4)).max(min_dc).min(max_dc);
                a7 = (dcs[ 7] + dc_pred(a4, a5, a6)).max(min_dc).min(max_dc);

                a8 = (dcs[ 8] + dc_pred(dst[7], dst[13], a2)).max(min_dc).min(max_dc);
                a9 = (dcs[ 9] + dc_pred(a2, a3, a8)).max(min_dc).min(max_dc);
                aA = (dcs[10] + dc_pred(dst[13], dst[15], a8)).max(min_dc).min(max_dc);
                aB = (dcs[11] + dc_pred(a8, a9, aA)).max(min_dc).min(max_dc);

                aC = (dcs[12] + dc_pred(a3, a6, a9)).max(min_dc).min(max_dc);
                aD = (dcs[13] + dc_pred(a6, a7, aC)).max(min_dc).min(max_dc);
                aE = (dcs[14] + dc_pred(a9, aB, aC)).max(min_dc).min(max_dc);
                aF = (dcs[15] + dc_pred(aC, aD, aE)).max(min_dc).min(max_dc);
            } else {
                a0 = (dcs[ 0] + dc_pred(prev_dc[3], prev_dc[4], dst[5])).max(min_dc).min(max_dc);
                a1 = (dcs[ 1] + dc_pred(prev_dc[4], prev_dc[5], a0)).max(min_dc).min(max_dc);
                a2 = (dcs[ 2] + dc_pred(dst[5], dst[7], a0)).max(min_dc).min(max_dc);
                a3 = (dcs[ 3] + dc_pred(a0, a1, a2)).max(min_dc).min(max_dc);

                a4 = (dcs[ 4] + dc_pred(prev_dc[5], prev_dc[6], a1)).max(min_dc).min(max_dc);
                a5 = (dcs[ 5] + dc_pred(prev_dc[6], prev_dc[7], a4)).max(min_dc).min(max_dc);
                a6 = (dcs[ 6] + dc_pred(a1, a3, a4)).max(min_dc).min(max_dc);
                a7 = (dcs[ 7] + dc_pred(a4, a5, a6)).max(min_dc).min(max_dc);

                a8 = (dcs[ 8] + dc_pred(dst[7], dst[13], a2)).max(min_dc).min(max_dc);
                a9 = (dcs[ 9] + dc_pred(a2, a3, a8)).max(min_dc).min(max_dc);
                aA = (dcs[10] + dc_pred(dst[13], dst[15], a8)).max(min_dc).min(max_dc);
                aB = (dcs[11] + dc_pred(a8, a9, aA)).max(min_dc).min(max_dc);

                aC = (dcs[12] + dc_pred(a3, a6, a9)).max(min_dc).min(max_dc);
                aD = (dcs[13] + dc_pred(a6, a7, aC)).max(min_dc).min(max_dc);
                aE = (dcs[14] + dc_pred(a9, aB, aC)).max(min_dc).min(max_dc);
                aF = (dcs[15] + dc_pred(aC, aD, aE)).max(min_dc).min(max_dc);
            }
        }
        dst[ 0] = a0;
        dst[ 1] = a1;
        dst[ 2] = a2;
        dst[ 3] = a3;
        dst[ 4] = a4;
        dst[ 5] = a5;
        dst[ 6] = a6;
        dst[ 7] = a7;
        dst[ 8] = a8;
        dst[ 9] = a9;
        dst[10] = aA;
        dst[11] = aB;
        dst[12] = aC;
        dst[13] = aD;
        dst[14] = aE;
        dst[15] = aF;
        let new_dc = &mut self.new_dcs[bx * 4 + 4..];
        new_dc[0] = aA;
        new_dc[1] = aB;
        new_dc[2] = aE;
        new_dc[3] = aF;
    }
    #[allow(non_snake_case)]
    fn predict_inter(&mut self, min_dc: i32, max_dc: i32) {
        let dst = &mut self.dcs;
        let dcs = &self.dc_buf;
        let a0 = (dcs[ 0] + 0).max(min_dc).min(max_dc);
        let a1 = (dcs[ 1] + a0).max(min_dc).min(max_dc);
        let a2 = (dcs[ 2] + dc_pred2(a0, a1)).max(min_dc).min(max_dc);
        let a3 = (dcs[ 3] + dc_pred(a0, a1, a2)).max(min_dc).min(max_dc);

        let a4 = (dcs[ 4] + dc_pred2(a1, a3)).max(min_dc).min(max_dc);
        let a5 = (dcs[ 5] + a4).max(min_dc).min(max_dc);
        let a6 = (dcs[ 6] + dc_pred(a1, a3, a4)).max(min_dc).min(max_dc);
        let a7 = (dcs[ 7] + dc_pred(a4, a5, a6)).max(min_dc).min(max_dc);

        let a8 = (dcs[ 8] + dc_pred2(a2, a3)).max(min_dc).min(max_dc);
        let a9 = (dcs[ 9] + dc_pred(a2, a3, a8)).max(min_dc).min(max_dc);
        let aA = (dcs[10] + dc_pred2(a8, a9)).max(min_dc).min(max_dc);
        let aB = (dcs[11] + dc_pred(a8, a9, aA)).max(min_dc).min(max_dc);

        let aC = (dcs[12] + dc_pred(a3, a6, a9)).max(min_dc).min(max_dc);
        let aD = (dcs[13] + dc_pred(a6, a7, aC)).max(min_dc).min(max_dc);
        let aE = (dcs[14] + dc_pred(a9, aB, aC)).max(min_dc).min(max_dc);
        let aF = (dcs[15] + dc_pred(aC, aD, aE)).max(min_dc).min(max_dc);

        dst[ 0] = a0;
        dst[ 1] = a1;
        dst[ 2] = a2;
        dst[ 3] = a3;
        dst[ 4] = a4;
        dst[ 5] = a5;
        dst[ 6] = a6;
        dst[ 7] = a7;
        dst[ 8] = a8;
        dst[ 9] = a9;
        dst[10] = aA;
        dst[11] = aB;
        dst[12] = aC;
        dst[13] = aD;
        dst[14] = aE;
        dst[15] = aF;
    }
}

impl DCInfo for CDCInfo {
    fn decode_dcs(&mut self, br: &mut BitReader, q: u8) -> DecoderResult<bool> {
        decode_dcs(br, &mut self.dc_buf, q)
    }
    fn predict(&mut self, bx: usize, is_top: bool, is_left: bool, min_dc: i32, max_dc: i32) {
        let prev_dc = &self.prev_dcs[bx * 2..];
        let dst = &mut self.dcs;
        let dcs = &self.dc_buf;

        let a0; let a1; let a2; let a3;
        if is_left {
            if is_top {
                let add = 1 << 10;//if has_prev_dc { 1 << 10 } else { 0 };
                a0 = (dcs[ 0] + add).max(min_dc).min(max_dc);
                a1 = (dcs[ 1] + a0).max(min_dc).min(max_dc);
            } else {
                a0 = (dcs[ 0] + dc_pred2(prev_dc[2], prev_dc[3])).max(min_dc).min(max_dc);
                a1 = (dcs[ 1] + dc_pred(prev_dc[2], prev_dc[3], a0)).max(min_dc).min(max_dc);
            }
            a2 = (dcs[ 2] + dc_pred2(a0, a1)).max(min_dc).min(max_dc);
            a3 = (dcs[ 3] + dc_pred(a0, a1, a2)).max(min_dc).min(max_dc);
        } else {
            if is_top {
                a0 = (dcs[ 0] + dc_pred2(dst[1], dst[3])).max(min_dc).min(max_dc);
                a1 = (dcs[ 1] + a0).max(min_dc).min(max_dc);
            } else {
                a0 = (dcs[ 0] + dc_pred(prev_dc[1], prev_dc[2], dst[1])).max(min_dc).min(max_dc);
                a1 = (dcs[ 1] + dc_pred(prev_dc[2], prev_dc[3], a0)).max(min_dc).min(max_dc);
            }
            a2 = (dcs[ 2] + dc_pred(dst[1], dst[3], a0)).max(min_dc).min(max_dc);
            a3 = (dcs[ 3] + dc_pred(a0, a1, a2)).max(min_dc).min(max_dc);
        }
        dst[0] = a0;
        dst[1] = a1;
        dst[2] = a2;
        dst[3] = a3;
        let new_dc = &mut self.new_dcs[bx * 2 + 2..];
        new_dc[0] = a2;
        new_dc[1] = a3;
    }
    #[allow(clippy::identity_op)]
    fn predict_inter(&mut self, min_dc: i32, max_dc: i32) {
        let dst = &mut self.dcs;
        let dcs = &self.dc_buf;

        let a0 = (dcs[ 0] + 0).max(min_dc).min(max_dc);
        let a1 = (dcs[ 1] + a0).max(min_dc).min(max_dc);
        let a2 = (dcs[ 2] + dc_pred2(a0, a1)).max(min_dc).min(max_dc);
        let a3 = (dcs[ 3] + dc_pred(a0, a1, a2)).max(min_dc).min(max_dc);

        dst[0] = a0;
        dst[1] = a1;
        dst[2] = a2;
        dst[3] = a3;
    }
}

fn decode_dcs(br: &mut BitReader, dcs: &mut [i32], q: u8) -> DecoderResult<bool> {
    if !br.read_bool()? {
        for dc in dcs.iter_mut() {
            *dc = 0;
        }
        Ok(false)
    } else {
        let quant = BINK2_QUANT_DC[q.max(8) as usize];
        for dc in dcs.iter_mut() {
            *dc                                 = br.read_bink2_code_zero()?;
            if *dc != 0 {
                *dc = (*dc * quant + 0x1FF) >> 10;
                if br.read_bool()? {
                    *dc = -*dc;
                }
            }
        }
        Ok(true)
    }
}
#[derive(Default)]
struct MVInfo {
    mvs:        Vec<MV>,
    stride:     usize,
    mv_off:     usize,
    prev_off:   usize,
}

#[allow(clippy::collapsible_else_if)]
#[allow(clippy::identity_op)]
impl MVInfo {
    fn resize(&mut self, bw: usize) {
        self.stride     = bw * 4;
        self.mv_off     = 0;
        self.prev_off   = self.stride;
        self.mvs.resize(self.stride * 2, ZERO_MV);
    }
    fn update_line(&mut self) {
        mem::swap(&mut self.mv_off, &mut self.prev_off);
    }
    fn decode_mv_new(&mut self, br: &mut BitReader, mv_cb: &Codebook<i8>, bx: usize, is_top: bool, is_left: bool) -> DecoderResult<()> {
        let num_mv = if br.read_bool()? { 1 } else { 4 };
        let mv = &mut self.mvs;
        let mv_pos = self.mv_off + bx * 4;
        let ref_pos = self.prev_off + bx * 4;

        for comp in 0..2 {
            for i in 0..num_mv {
                let val;
                let raw_val                         = br.read_cb(mv_cb)?;
                if raw_val != BINK2_MV_ESC {
                    val = raw_val as i16;
                } else {
                    let len                         = br.read_code(UintCodeType::LimitedUnary(12, 1))? as u8;
                    let nlen = len + 4;
                    let uval = (br.read(nlen)? as i32) + (1 << nlen) - 1;
                    if (uval & 1) != 0 {
                        val = (-(uval >> 1) - 1) as i16;
                    } else {
                        val = (uval >> 1) as i16;
                    }
                }
                if comp == 0 {
                    mv[mv_pos + i].x = val;
                } else {
                    mv[mv_pos + i].y = val;
                }
            }
        }

        if num_mv > 1 {
            if is_top {
                if !is_left {
                    let mv0 = mv[mv_pos + 0] + MV::pred(mv[mv_pos - 4], mv[mv_pos - 3], mv[mv_pos - 1]);
                    mv[mv_pos + 0] = mv0;
                    let mv2 = mv[mv_pos + 2] + MV::pred(mv[mv_pos - 3], mv[mv_pos - 1], mv[mv_pos + 0]);
                    mv[mv_pos + 2] = mv2;
                    let mv1 = mv[mv_pos + 1] + MV::pred(mv[mv_pos - 3], mv[mv_pos + 0], mv[mv_pos + 2]);
                    mv[mv_pos + 1] = mv1;
                    let mv3 = mv[mv_pos + 3] + MV::pred(mv[mv_pos + 0], mv[mv_pos + 1], mv[mv_pos + 2]);
                    mv[mv_pos + 3] = mv3;
                } else {
                    let mv1 = mv[mv_pos + 1] + mv[mv_pos + 0];
                    mv[mv_pos + 1] = mv1;
                    let mv2 = mv[mv_pos + 2] + mv[mv_pos + 0];
                    mv[mv_pos + 2] = mv2;
                    let mv3 = mv[mv_pos + 3] + MV::pred(mv[mv_pos + 0], mv[mv_pos + 1], mv[mv_pos + 2]);
                    mv[mv_pos + 3] = mv3;
                }
            } else {
                if is_left {
                    let mv0 = mv[mv_pos + 0] + MV::pred(mv[ref_pos + 0], mv[ref_pos + 2], mv[ref_pos + 3]);
                    mv[mv_pos + 0] = mv0;
                    let mv1 = mv[mv_pos + 1] + MV::pred(mv[ref_pos + 2], mv[ref_pos + 3], mv[mv_pos + 0]);
                    mv[mv_pos + 1] = mv1;
                    let mv2 = mv[mv_pos + 2] + MV::pred(mv[ref_pos + 2], mv[mv_pos + 0],  mv[mv_pos + 1]);
                    mv[mv_pos + 2] = mv2;
                    let mv3 = mv[mv_pos + 3] + MV::pred(mv[mv_pos + 0],  mv[mv_pos + 1],  mv[mv_pos + 2]);
                    mv[mv_pos + 3] = mv3;
                } else {
                    let mv0 = mv[mv_pos + 0] + MV::pred(mv[ref_pos + 2], mv[ref_pos - 1], mv[mv_pos - 3]);
                    mv[mv_pos + 0] = mv0;
                    let mv1 = mv[mv_pos + 1] + MV::pred(mv[ref_pos + 2], mv[ref_pos + 3], mv[mv_pos + 0]);
                    mv[mv_pos + 1] = mv1;
                    let mv2 = mv[mv_pos + 2] + MV::pred(mv[mv_pos - 3],  mv[mv_pos - 1],  mv[mv_pos + 0]);
                    mv[mv_pos + 2] = mv2;
                    let mv3 = mv[mv_pos + 3] + MV::pred(mv[mv_pos + 0],  mv[mv_pos + 1],  mv[mv_pos + 2]);
                    mv[mv_pos + 3] = mv3;
                }
            }
        } else {
            let mut mv_pred = mv[mv_pos + 0];
            if is_top {
                if !is_left {
                    mv_pred += MV::pred(mv[mv_pos - 4],  mv[mv_pos - 3],  mv[mv_pos - 1]);
                }
            } else {
                if !is_left {
                    mv_pred += MV::pred(mv[ref_pos - 1], mv[ref_pos + 2], mv[mv_pos - 3]);
                } else {
                    mv_pred += MV::pred(mv[ref_pos + 0], mv[ref_pos + 2], mv[ref_pos + 3]);
                }
            }
            mv[mv_pos + 0] = mv_pred;
            mv[mv_pos + 1] = mv_pred;
            mv[mv_pos + 2] = mv_pred;
            mv[mv_pos + 3] = mv_pred;
        }
        Ok(())
    }
    fn decode_mv_old(&mut self, br: &mut BitReader, bx: usize, is_top: bool, is_left: bool) -> DecoderResult<()> {
        let mv = &mut self.mvs;
        let mv_pos = self.mv_off + bx * 4;
        let ref_pos = self.prev_off + bx * 4;

        let mut basex = 0;
        let mut basey = 0;
        for comp in 0..2 {
            let mut bits                            = br.read(3)? as u8;
            if bits == 7 {
                bits                                += br.read(2)? as u8;
            }
            let mut mv_c: [i16; 4] = [0; 4];
            if bits > 0 {
                for mv in mv_c.iter_mut() {
                    *mv                             = br.read(bits)? as i16;
                }
                for mv in mv_c.iter_mut() {
                    if (*mv != 0) && br.read_bool()? {
                        *mv = -*mv;
                    }
                }
            }
            if is_top && is_left {
                let mut val                         = br.read(5)? as i16;
                if (val != 0) && br.read_bool()? {
                    val = -val;
                }
                if comp == 0 {
                    basex = val;
                } else {
                    basey = val;
                }
            }
            if comp == 0 {
                for i in 0..4 {
                    mv[mv_pos + i].x = mv_c[i];
                }
            } else {
                for i in 0..4 {
                    mv[mv_pos + i].y = mv_c[i];
                }
            }
        }
        if !is_top {
            if is_left {
                let mv0 = mv[mv_pos + 0] + MV::pred(mv[ref_pos + 0], mv[ref_pos + 2], mv[ref_pos + 3]);
                mv[mv_pos + 0] = mv0;
                let mv1 = mv[mv_pos + 1] + MV::pred(mv[ref_pos + 2], mv[ref_pos + 3], mv[mv_pos + 0]);
                mv[mv_pos + 1] = mv1;
                let mv2 = mv[mv_pos + 2] + MV::pred(mv[ref_pos + 2], mv[mv_pos + 0],  mv[mv_pos + 1]);
                mv[mv_pos + 2] = mv2;
                let mv3 = mv[mv_pos + 3] + MV::pred(mv[mv_pos + 0],  mv[mv_pos + 1],  mv[mv_pos + 2]);
                mv[mv_pos + 3] = mv3;
            } else {
                let mv0 = mv[mv_pos + 0] + MV::pred(mv[ref_pos - 1], mv[ref_pos + 2], mv[mv_pos - 3]);
                mv[mv_pos + 0] = mv0;
                let mv1 = mv[mv_pos + 1] + MV::pred(mv[ref_pos + 2], mv[ref_pos + 3], mv[mv_pos + 0]);
                mv[mv_pos + 1] = mv1;
                let mv2 = mv[mv_pos + 2] + MV::pred(mv[ref_pos + 2], mv[mv_pos + 0],  mv[mv_pos + 1]);
                mv[mv_pos + 2] = mv2;
                let mv3 = mv[mv_pos + 3] + MV::pred(mv[mv_pos + 0],  mv[mv_pos + 1],  mv[mv_pos + 2]);
                mv[mv_pos + 3] = mv3;
            }
        } else {
            if is_left {
                let base_mv = MV { x: basex * 16, y: basey * 16 };
                mv[mv_pos + 0] += base_mv;
                mv[mv_pos + 1] += base_mv;
                mv[mv_pos + 2] += base_mv;
                mv[mv_pos + 3] += base_mv;
            } else {
                let mv0 = mv[mv_pos + 0] + MV::pred(mv[mv_pos - 4], mv[mv_pos - 3], mv[mv_pos - 1]);
                mv[mv_pos + 0] = mv0;
                let mv2 = mv[mv_pos + 2] + MV::pred(mv[mv_pos - 3], mv[mv_pos - 1], mv[mv_pos + 0]);
                mv[mv_pos + 2] = mv2;
                let mv1 = mv[mv_pos + 1] + MV::pred(mv[mv_pos - 3], mv[mv_pos + 0], mv[mv_pos + 2]);
                mv[mv_pos + 1] = mv1;
                let mv3 = mv[mv_pos + 3] + MV::pred(mv[mv_pos + 0], mv[mv_pos + 1], mv[mv_pos + 2]);
                mv[mv_pos + 3] = mv3;
            }
        }
        Ok(())
    }

    fn predict_mvs(&mut self, bx: usize, is_top: bool, is_left: bool, is_new: bool) {
        let mv = &mut self.mvs;
        let mv_pos = self.mv_off + bx * 4;
        let ref_pos = self.prev_off + bx * 4;

        if is_top {
            if is_left {
                for i in 0..4 {
                    mv[mv_pos + i] = ZERO_MV;
                }
            } else {
                mv[mv_pos + 0] = MV::pred(mv[mv_pos - 4], mv[mv_pos - 3], mv[mv_pos - 1]);
                mv[mv_pos + 2] = MV::pred(mv[mv_pos - 3], mv[mv_pos - 1], mv[mv_pos + 0]);
                mv[mv_pos + 1] = MV::pred(mv[mv_pos - 1], mv[mv_pos + 0], mv[mv_pos + 2]);
                mv[mv_pos + 3] = MV::pred(mv[mv_pos + 0], mv[mv_pos + 1], mv[mv_pos + 2]);
            }
        } else {
            if is_left {
                mv[mv_pos + 0] = MV::pred(mv[ref_pos + 0], mv[ref_pos + 2], mv[ref_pos + 3]);
                mv[mv_pos + 1] = MV::pred(mv[mv_pos + 0],  mv[ref_pos + 2], mv[ref_pos + 3]);
                mv[mv_pos + 2] = MV::pred(mv[ref_pos + 2], mv[mv_pos + 1],  mv[mv_pos + 0]);
                mv[mv_pos + 3] = MV::pred(mv[mv_pos + 0],  mv[mv_pos + 1],  mv[mv_pos + 2]);
            } else {
                mv[mv_pos + 0] = MV::pred(mv[mv_pos - 3], mv[ref_pos - 1], mv[ref_pos + 2]);
                mv[mv_pos + 1] = MV::pred(mv[mv_pos + 0], mv[ref_pos + 2], mv[ref_pos + 3]);
                if is_new {
                    mv[mv_pos + 2] = MV::pred(mv[mv_pos - 3], mv[mv_pos - 1],  mv[mv_pos + 0]);
                } else {
                    mv[mv_pos + 2] = MV::pred(mv[ref_pos + 2], mv[mv_pos + 1],  mv[mv_pos + 0]);
                }
                mv[mv_pos + 3] = MV::pred(mv[mv_pos + 0], mv[mv_pos + 1],  mv[mv_pos + 2]);
            }
        }
    }

    fn get_mv(&self, bx: usize, blk: usize) -> MV { self.mvs[self.mv_off + bx * 4 + blk] }
    fn zero(&mut self, bx: usize) {
        let pos = self.mv_off + bx * 4;
        self.mvs[pos + 0] = ZERO_MV;
        self.mvs[pos + 1] = ZERO_MV;
        self.mvs[pos + 2] = ZERO_MV;
        self.mvs[pos + 3] = ZERO_MV;
    }
}

struct Bink2Codes {
    ac_cb1:     Codebook<u8>,
    ac_cb2:     Codebook<u8>,
    mv_cb:      Codebook<i8>,
    val_cb1:    Codebook<u8>,
    val_cb2:    Codebook<u8>,
    skip_cb1:   Codebook<u8>,
    skip_cb2:   Codebook<u8>,
    quant_cb:   Codebook<u8>,
}

fn map_ac(idx: usize) -> u8 { idx as u8 }

impl Default for Bink2Codes {
    fn default() -> Self {
        let mut cbr = TableCodebookDescReader::new(BINK2_AC_CODES1, BINK2_AC_BITS1, map_ac);
        let ac_cb1 = Codebook::new(&mut cbr, CodebookMode::LSB).unwrap();
        let mut cbr = TableCodebookDescReader::new(BINK2_AC_CODES2, BINK2_AC_BITS2, map_ac);
        let ac_cb2 = Codebook::new(&mut cbr, CodebookMode::LSB).unwrap();
        let mut cbr = TableCodebookDescReader::new(BINK2_MV_CODES, BINK2_MV_BITS, |idx| BINK2_MV_SYMS[idx]);
        let mv_cb = Codebook::new(&mut cbr, CodebookMode::LSB).unwrap();
        let mut cbr = TableCodebookDescReader::new(BINK2_AC_VAL_CODES1, BINK2_AC_VAL_BITS1, map_ac);
        let val_cb1 = Codebook::new(&mut cbr, CodebookMode::LSB).unwrap();
        let mut cbr = TableCodebookDescReader::new(BINK2_AC_VAL_CODES2, BINK2_AC_VAL_BITS2, map_ac);
        let val_cb2 = Codebook::new(&mut cbr, CodebookMode::LSB).unwrap();
        let mut cbr = TableCodebookDescReader::new(BINK2_AC_SKIP_CODES1, BINK2_AC_SKIP_BITS1, map_ac);
        let skip_cb1 = Codebook::new(&mut cbr, CodebookMode::LSB).unwrap();
        let mut cbr = TableCodebookDescReader::new(BINK2_AC_SKIP_CODES2, BINK2_AC_SKIP_BITS2, map_ac);
        let skip_cb2 = Codebook::new(&mut cbr, CodebookMode::LSB).unwrap();
        let mut cbr = TableCodebookDescReader::new(BINK2_QUANT_CODES, BINK2_QUANT_BITS, map_ac);
        let quant_cb = Codebook::new(&mut cbr, CodebookMode::LSB).unwrap();
        Self { ac_cb1, ac_cb2, mv_cb, val_cb1, val_cb2, skip_cb1, skip_cb2, quant_cb }
    }
}

#[derive(Default)]
struct Bink2Decoder {
    info:       NACodecInfoRef,
    ips:        IPShuffler,

    version:    u32,
    is_kb2d:    bool,
    is_kb2i:    bool, // KB2i or newer
    has_alpha:  bool,
    slice_h:    [usize; 8],
    num_slices: usize,

    key_frame:  bool,
    cur_w:      usize,
    cur_h:      usize,
    qinfo:      QuantInfo,
    y_dcs:      YDCInfo,
    u_dcs:      CDCInfo,
    v_dcs:      CDCInfo,
    a_dcs:      YDCInfo,
    mvs:        MVInfo,

    codes:      Bink2Codes,
}

#[allow(clippy::erasing_op)]
impl Bink2Decoder {
    fn new() -> Self {
        Self::default()
    }

    #[allow(clippy::cognitive_complexity)]
    #[allow(clippy::identity_op)]
    #[allow(clippy::needless_range_loop)]
    fn decode_frame_new(&mut self, br: &mut BitReader, buf: &mut NAVideoBuffer<u8>, is_intra: bool) -> DecoderResult<()> {
        let (stride_y, stride_u, stride_v, stride_a) = (buf.get_stride(0), buf.get_stride(1), buf.get_stride(2), buf.get_stride(3));
        let (mut off_y, mut off_u, mut off_v, mut off_a) = (buf.get_offset(0), buf.get_offset(1), buf.get_offset(2), buf.get_offset(3));
        let (ooff_y, ooff_u, ooff_v, ooff_a) = (off_y, off_u, off_v, off_a);
        let (width, height) = buf.get_dimensions(0);
        let kb2i = self.is_kb2i;
        let data = buf.get_data_mut().unwrap();
        let dst = data.as_mut_slice();
        let bw = (width  + 31) >> 5;
        let bheight = (height + 31) >> 5;
        self.cur_w = (width + 7) & !7;
        self.cur_h = ((height + 7) & !7) >> 1;

        let frame_flags                         = br.read(32)?;
        let mut offsets: [u32; 7] = [0; 7];
        for offset in offsets[..self.num_slices-1].iter_mut() {
            *offset                             = br.read(32)?;
        }
        let mut do_alpha = self.has_alpha;
        if (frame_flags & 0x80000) != 0 && self.has_alpha {
            do_alpha = false;
            let fillval = (frame_flags >> 24) as u8;
            let aplane = &mut dst[off_a..][..stride_a * bheight * 32];
            for el in aplane.iter_mut() {
                *el = fillval;
            }
        }
        let mut row_flags: Vec<bool> = Vec::with_capacity(bheight * 4);
        let mut col_flags: Vec<bool> = Vec::with_capacity(bw * 4);
        if (frame_flags & 0x10000) != 0 {
            if (frame_flags & 0x8000) == 0 {
                let len = (height + 15) >> 4;
                decode_flags(br, &mut row_flags, 1, len * 2 - 1)?;
            }
            if (frame_flags & 0x4000) == 0 {
                let len = (width + 15) >> 4;
                decode_flags(br, &mut col_flags, 1, len * 2 - 1)?;
            }
        }
        row_flags.resize(bheight * 4, false);
        col_flags.resize(bw * 4, false);
        //store frame_flags  * 8 & 0x7F8

        let mut start_by = 0;
        for slice_no in 0..self.num_slices {
            let end_by = self.slice_h[slice_no];
            if slice_no != 0 {
                br.seek(offsets[slice_no - 1] * 8)?;
            }
            off_y = ooff_y + stride_y * start_by * 32;
            off_u = ooff_u + stride_u * start_by * 16;
            off_v = ooff_v + stride_v * start_by * 16;
            off_a = ooff_a + stride_a * start_by * 32;

            let mut row_state = frame_flags & 0x2E000;
            if is_intra {
                row_state |= 0x10000;
            }
            self.qinfo.resize(bw);
            self.y_dcs.resize(bw);
            self.u_dcs.resize(bw);
            self.v_dcs.resize(bw);
            self.a_dcs.resize(bw);
            self.mvs.resize(bw);
            for by in start_by..end_by {
                let mut cbp_y = 0;
                let mut cbp_u = 0;
                let mut cbp_v = 0;
                let mut cbp_a = 0;
                let mut cbp_y_p = 0;
                let mut cbp_u_p = 0;
                let mut cbp_v_p = 0;
                let mut cbp_a_p = 0;
                let mut q_y = 8;
                let mut q_u = 8;
                let mut q_v = 8;
                let mut q_a = 8;
                let mut q_y_p = 8;
                let mut q_u_p = 8;
                let mut q_v_p = 8;
                let mut q_a_p = 8;
                let rflags = (row_flags[by] as u32) * 4;
                row_state = (row_state & 0x3FFFFFF) | ((row_state >> 4) & 0xC000000) | (rflags << 28);
                if by == start_by {
                    row_state |= 0x80;
//                } else {
//                    row_state |= 0x8;
                }
                if by + 2 >= end_by {
                    row_state |= 0x100;
                }

                let mut btype_lru: [u8; 4] = [ 2, 3, 1, 0 ];
                let mut edge_state = 0;
                let is_top = by == start_by;
                for bx in 0..bw {
                    let mut blk_state = row_state | (edge_state & 0x3FC0000);
                    if bx == 0 {
                        blk_state |= 0x20;
                    }
                    if bx + 2 < bw {
                        blk_state |= 0x40;
                    }
                    if (bx & 1) != 0 {
                        blk_state |= 0x200;
                    }
                    let clflags = (col_flags[bx] as u32) * 4;
                    let edge_state_c = ((blk_state >> 4) & 0x3C0000) | (blk_state & 0xFC03FFFF) | ((clflags & 0xF) << 22);
                    let edge_state_y = (frame_flags & 0x40000) | (blk_state & 0x3FFFF);
                    edge_state = edge_state_c;

                    //let is_top = (edge_state & 0x88) != 0;
                    let is_left = bx == 0; //(edge_state & 0x20) != 0;

                    let btype;
                    if is_intra {
                        btype = 0;
                    } else if (blk_state & 0x2000) != 0 {
                        let val                     = br.read_code(UintCodeType::LimitedUnary(3, 1))?;
                        match val {
                            0 => {
                                btype = btype_lru[0];
                            },
                            1 => {
                                btype = btype_lru[1];
                                btype_lru[1] = btype_lru[0];
                                btype_lru[0] = btype;
                            },
                            2 => {
                                btype = btype_lru[3];
                                btype_lru[3] = btype_lru[2];
                                btype_lru[2] = btype;
                            },
                            3 => {
                                btype = btype_lru[2];
                                btype_lru[2] = btype_lru[1];
                                btype_lru[1] = btype;
                            },
                            _ => unreachable!(),
                        };
                    } else {
                        btype                       = br.read(2)? as u8;
                    }
                    if !is_intra {
                        let q = self.qinfo.pred_quant(bx * 2, is_top);
                        self.qinfo.set_quant(bx * 2, q);
                        let q = self.qinfo.pred_quant(bx * 2 + 1, is_top);
                        self.qinfo.set_quant(bx * 2 + 1, q);
                    }
                    match btype {
                        0 => { // intra
                            if (frame_flags & 0x2000) != 0 {
                                let mut yblk: [[[i32; 64]; 4]; 4] = [[[0; 64]; 4]; 4];
                                let mut ublk: [[i32; 64]; 4] = [[0; 64]; 4];
                                let mut vblk: [[i32; 64]; 4] = [[0; 64]; 4];
                                let q = get_new_quant(br, self.qinfo.pred_quant(bx * 2, is_top))?;
                                self.qinfo.set_quant(bx * 2, q);
                                cbp_y = decode_luma_intra(br, &self.codes, cbp_y, q, &mut yblk, edge_state_y, &mut self.y_dcs, bx)?;
                                cbp_v = decode_chroma_intra(br, &self.codes, cbp_v, q, &mut vblk, edge_state_c, &mut self.v_dcs, bx)?;
                                cbp_u = decode_chroma_intra(br, &self.codes, cbp_u, q, &mut ublk, edge_state_c, &mut self.u_dcs, bx)?;
                                if do_alpha {
                                    let mut ablk: [[[i32; 64]; 4]; 4] = [[[0; 64]; 4]; 4];
                                    cbp_a = decode_luma_intra(br, &self.codes, cbp_a, q, &mut ablk, edge_state_y, &mut self.a_dcs, bx)?;
                                    Bink2DSP::put_mb4(dst, off_a + bx * 32 +  0 +  0 * stride_a, stride_a, &mut ablk[0]);
                                    Bink2DSP::put_mb4(dst, off_a + bx * 32 + 16 +  0 * stride_a, stride_a, &mut ablk[1]);
                                    Bink2DSP::put_mb4(dst, off_a + bx * 32 +  0 + 16 * stride_a, stride_a, &mut ablk[2]);
                                    Bink2DSP::put_mb4(dst, off_a + bx * 32 + 16 + 16 * stride_a, stride_a, &mut ablk[3]);
                                }
//if smth else decode one more y
                                Bink2DSP::put_mb4(dst, off_y + bx * 32 +  0 +  0 * stride_y, stride_y, &mut yblk[0]);
                                Bink2DSP::put_mb4(dst, off_y + bx * 32 + 16 +  0 * stride_y, stride_y, &mut yblk[1]);
                                Bink2DSP::put_mb4(dst, off_y + bx * 32 +  0 + 16 * stride_y, stride_y, &mut yblk[2]);
                                Bink2DSP::put_mb4(dst, off_y + bx * 32 + 16 + 16 * stride_y, stride_y, &mut yblk[3]);
                                Bink2DSP::put_mb4(dst, off_u + bx * 16, stride_u, &mut ublk);
                                Bink2DSP::put_mb4(dst, off_v + bx * 16, stride_v, &mut vblk);
                            } else {
                                let mut yblk: [[[f32; 64]; 4]; 4] = [[[0.0; 64]; 4]; 4];
                                let mut ublk: [[f32; 64]; 4] = [[0.0; 64]; 4];
                                let mut vblk: [[f32; 64]; 4] = [[0.0; 64]; 4];
                                if self.is_kb2d {
                                    decode_luma_intra_kb2d(br, &mut yblk, edge_state_y, &mut self.y_dcs, bx)?;
                                    decode_chroma_intra_kb2d(br, &mut vblk, edge_state_c, &mut self.v_dcs, bx)?;
                                    decode_chroma_intra_kb2d(br, &mut ublk, edge_state_c, &mut self.u_dcs, bx)?;
                                } else {
                                cbp_y = decode_luma_intra_old(br, &self.codes, cbp_y, &mut yblk, edge_state_y, &mut self.y_dcs, bx, &mut q_y)?;
                                cbp_v = decode_chroma_intra_old(br, &self.codes, cbp_v, &mut vblk, edge_state_c, &mut self.v_dcs, bx, &mut q_v)?;
                                cbp_u = decode_chroma_intra_old(br, &self.codes, cbp_u, &mut ublk, edge_state_c, &mut self.u_dcs, bx, &mut q_u)?;
                                }
                                if do_alpha {
                                    let mut ablk: [[[f32; 64]; 4]; 4] = [[[0.0; 64]; 4]; 4];
                                    cbp_a = decode_luma_intra_old(br, &self.codes, cbp_a, &mut ablk, edge_state_y, &mut self.a_dcs, bx, &mut q_a)?;
                                    Bink2DSP::put_mb4_old(dst, off_a + bx * 32 +  0 +  0 * stride_a, stride_a, &mut ablk[0]);
                                    Bink2DSP::put_mb4_old(dst, off_a + bx * 32 + 16 +  0 * stride_a, stride_a, &mut ablk[1]);
                                    Bink2DSP::put_mb4_old(dst, off_a + bx * 32 +  0 + 16 * stride_a, stride_a, &mut ablk[2]);
                                    Bink2DSP::put_mb4_old(dst, off_a + bx * 32 + 16 + 16 * stride_a, stride_a, &mut ablk[3]);
                                }
                                Bink2DSP::put_mb4_old(dst, off_y + bx * 32 +  0 +  0 * stride_y, stride_y, &mut yblk[0]);
                                Bink2DSP::put_mb4_old(dst, off_y + bx * 32 + 16 +  0 * stride_y, stride_y, &mut yblk[1]);
                                Bink2DSP::put_mb4_old(dst, off_y + bx * 32 +  0 + 16 * stride_y, stride_y, &mut yblk[2]);
                                Bink2DSP::put_mb4_old(dst, off_y + bx * 32 + 16 + 16 * stride_y, stride_y, &mut yblk[3]);
                                Bink2DSP::put_mb4_old(dst, off_u + bx * 16, stride_u, &mut ublk);
                                Bink2DSP::put_mb4_old(dst, off_v + bx * 16, stride_v, &mut vblk);
                            }
                            if !is_intra {
                                self.mvs.predict_mvs(bx, is_top, is_left, (frame_flags & 0x2000) != 0);
                            }
                        },
                        1 => { // skip
                            if let Some(ref ref_pic) = self.ips.get_ref() {
                                for blk_no in 0..4 {
                                    let xoff = bx * 32 + (blk_no & 1) * 16;
                                    let yoff = by * 32 + (blk_no & 2) * 8;
                                    Bink2DSP::mc_luma(&mut dst[off_y..], stride_y, ref_pic, xoff, yoff, ZERO_MV, 0)?;
                                    Bink2DSP::mc_chroma(&mut dst[off_u..], stride_u, ref_pic, xoff >> 1, yoff >> 1, ZERO_MV, 1, kb2i)?;
                                    Bink2DSP::mc_chroma(&mut dst[off_v..], stride_v, ref_pic, xoff >> 1, yoff >> 1, ZERO_MV, 2, kb2i)?;
                                    if do_alpha {
                                        Bink2DSP::mc_luma(&mut dst[off_a..], stride_a, ref_pic, xoff, yoff, ZERO_MV, 3)?;
                                    }
                                }
                            } else {
                                return Err(DecoderError::MissingReference);
                            }
                            self.mvs.zero(bx);
                        },
                        2 => { // motion
                            if (frame_flags & 0x2000) != 0 {
                                self.mvs.decode_mv_new(br, &self.codes.mv_cb, bx, is_top, is_left)?;
                            } else {
                                self.mvs.decode_mv_old(br, bx, is_top, is_left)?;
                            }
                            if let Some(ref ref_pic) = self.ips.get_ref() {
                                for blk_no in 0..4 {
                                    let xoff = bx * 32 + (blk_no & 1) * 16;
                                    let yoff = by * 32 + (blk_no & 2) * 8;
                                    let mv = self.mvs.get_mv(bx, blk_no);
                                    Bink2DSP::mc_luma(&mut dst[off_y..], stride_y, ref_pic, xoff, yoff, mv, 0)?;
                                    Bink2DSP::mc_chroma(&mut dst[off_u..], stride_u, ref_pic, xoff >> 1, yoff >> 1, mv, 1, kb2i)?;
                                    Bink2DSP::mc_chroma(&mut dst[off_v..], stride_v, ref_pic, xoff >> 1, yoff >> 1, mv, 2, kb2i)?;
                                    if do_alpha {
                                        Bink2DSP::mc_luma(&mut dst[off_a..], stride_a, ref_pic, xoff, yoff, mv, 3)?;
                                    }
                                }
                            } else {
                                return Err(DecoderError::MissingReference);
                            }
                        },
                        3 => { // inter
                            if (frame_flags & 0x2000) != 0 {
                                self.mvs.decode_mv_new(br, &self.codes.mv_cb, bx, is_top, is_left)?;
                            } else {
                                self.mvs.decode_mv_old(br, bx, is_top, is_left)?;
                            }
                            if let Some(ref ref_pic) = self.ips.get_ref() {
                                for blk_no in 0..4 {
                                    let xoff = bx * 32 + (blk_no & 1) * 16;
                                    let yoff = by * 32 + (blk_no & 2) * 8;
                                    let mv = self.mvs.get_mv(bx, blk_no);
                                    Bink2DSP::mc_luma(&mut dst[off_y..], stride_y, ref_pic, xoff, yoff, mv, 0)?;
                                    Bink2DSP::mc_chroma(&mut dst[off_u..], stride_u, ref_pic, xoff >> 1, yoff >> 1, mv, 1, kb2i)?;
                                    Bink2DSP::mc_chroma(&mut dst[off_v..], stride_v, ref_pic, xoff >> 1, yoff >> 1, mv, 2, kb2i)?;
                                    if do_alpha {
                                        Bink2DSP::mc_luma(&mut dst[off_a..], stride_a, ref_pic, xoff, yoff, mv, 3)?;
                                    }
                                }
                            } else {
                                return Err(DecoderError::MissingReference);
                            }
                            if (frame_flags & 0x2000) != 0 {
                                let mut yblk: [[[i32; 64]; 4]; 4] = [[[0; 64]; 4]; 4];
                                let mut ublk: [[i32; 64]; 4] = [[0; 64]; 4];
                                let mut vblk: [[i32; 64]; 4] = [[0; 64]; 4];
                                let q = get_new_quant(br, self.qinfo.pred_quant(bx * 2 + 1, is_top))?;
                                self.qinfo.set_quant(bx * 2 + 1, q);
                                cbp_y_p = decode_luma_inter(br, &self.codes, cbp_y_p, q, &mut yblk, edge_state_y, &mut self.y_dcs, kb2i)?;
                                if br.read_bool()? {
                                    cbp_v_p = decode_chroma_inter(br, &self.codes, cbp_v_p, q, &mut vblk, edge_state_y, &mut self.v_dcs, kb2i)?;
                                    cbp_u_p = decode_chroma_inter(br, &self.codes, cbp_u_p, q, &mut ublk, edge_state_y, &mut self.u_dcs, kb2i)?;
                                } else {
                                    cbp_v_p = 0;
                                    cbp_u_p = 0;
                                }
                                if do_alpha {
                                    let mut ablk: [[[i32; 64]; 4]; 4] = [[[0; 64]; 4]; 4];
                                    cbp_a_p = decode_luma_inter(br, &self.codes, cbp_a_p, q, &mut ablk, edge_state_y, &mut self.a_dcs, kb2i)?;
                                    Bink2DSP::add_mb4(dst, off_a + bx * 32 +  0 +  0 * stride_a, stride_a, &mut ablk[0]);
                                    Bink2DSP::add_mb4(dst, off_a + bx * 32 + 16 +  0 * stride_a, stride_a, &mut ablk[1]);
                                    Bink2DSP::add_mb4(dst, off_a + bx * 32 +  0 + 16 * stride_a, stride_a, &mut ablk[2]);
                                    Bink2DSP::add_mb4(dst, off_a + bx * 32 + 16 + 16 * stride_a, stride_a, &mut ablk[3]);
                                }
                                Bink2DSP::add_mb4(dst, off_y + bx * 32 +  0 +  0 * stride_y, stride_y, &mut yblk[0]);
                                Bink2DSP::add_mb4(dst, off_y + bx * 32 + 16 +  0 * stride_y, stride_y, &mut yblk[1]);
                                Bink2DSP::add_mb4(dst, off_y + bx * 32 +  0 + 16 * stride_y, stride_y, &mut yblk[2]);
                                Bink2DSP::add_mb4(dst, off_y + bx * 32 + 16 + 16 * stride_y, stride_y, &mut yblk[3]);
                                Bink2DSP::add_mb4(dst, off_u + bx * 16, stride_u, &mut ublk);
                                Bink2DSP::add_mb4(dst, off_v + bx * 16, stride_v, &mut vblk);
                            } else {
                                let mut yblk: [[[f32; 64]; 4]; 4] = [[[0.0; 64]; 4]; 4];
                                let mut ublk: [[f32; 64]; 4] = [[0.0; 64]; 4];
                                let mut vblk: [[f32; 64]; 4] = [[0.0; 64]; 4];
                                if self.is_kb2d {
                                    decode_luma_inter_kb2d(br, &mut yblk, &mut self.y_dcs)?;
                                    decode_chroma_inter_kb2d(br, &mut vblk, &mut ublk, &mut self.v_dcs, &mut self.u_dcs)?;
                                } else {
                                cbp_y_p = decode_luma_inter_old(br, &self.codes, cbp_y_p, &mut yblk, edge_state_y, &mut self.y_dcs, &mut q_y_p)?;
                                cbp_v_p = decode_chroma_inter_old(br, &self.codes, cbp_v_p, &mut vblk, edge_state_y, &mut self.v_dcs, &mut q_v_p)?;
                                cbp_u_p = decode_chroma_inter_old(br, &self.codes, cbp_u_p, &mut ublk, edge_state_y, &mut self.u_dcs, &mut q_u_p)?;
                                }
                                if do_alpha {
                                    let mut ablk: [[[f32; 64]; 4]; 4] = [[[0.0; 64]; 4]; 4];
                                    cbp_a_p = decode_luma_inter_old(br, &self.codes, cbp_a_p, &mut ablk, edge_state_y, &mut self.a_dcs, &mut q_a_p)?;
                                    Bink2DSP::add_mb4_old(dst, off_a + bx * 32 +  0 +  0 * stride_a, stride_a, &mut ablk[0]);
                                    Bink2DSP::add_mb4_old(dst, off_a + bx * 32 + 16 +  0 * stride_a, stride_a, &mut ablk[1]);
                                    Bink2DSP::add_mb4_old(dst, off_a + bx * 32 +  0 + 16 * stride_a, stride_a, &mut ablk[2]);
                                    Bink2DSP::add_mb4_old(dst, off_a + bx * 32 + 16 + 16 * stride_a, stride_a, &mut ablk[3]);
                                }
                                Bink2DSP::add_mb4_old(dst, off_y + bx * 32 +  0 +  0 * stride_y, stride_y, &mut yblk[0]);
                                Bink2DSP::add_mb4_old(dst, off_y + bx * 32 + 16 +  0 * stride_y, stride_y, &mut yblk[1]);
                                Bink2DSP::add_mb4_old(dst, off_y + bx * 32 +  0 + 16 * stride_y, stride_y, &mut yblk[2]);
                                Bink2DSP::add_mb4_old(dst, off_y + bx * 32 + 16 + 16 * stride_y, stride_y, &mut yblk[3]);
                                Bink2DSP::add_mb4_old(dst, off_u + bx * 16, stride_u, &mut ublk);
                                Bink2DSP::add_mb4_old(dst, off_v + bx * 16, stride_v, &mut vblk);
                            }
                        },
                        _ => unreachable!(),
                    };
                    if btype != 0 {
                        let src = &dst[off_y + bx * 32..];
                        let dc5 = Bink2DSP::calc_dc(&src[24..], stride_y);
                        let dc7 = Bink2DSP::calc_dc(&src[24 + stride_y * 8..], stride_y);
                        let dc13 = Bink2DSP::calc_dc(&src[24 + stride_y * 16..], stride_y);
                        let dc10 = Bink2DSP::calc_dc(&src[ 0 + stride_y * 24..], stride_y);
                        let dc11 = Bink2DSP::calc_dc(&src[ 8 + stride_y * 24..], stride_y);
                        let dc14 = Bink2DSP::calc_dc(&src[16 + stride_y * 24..], stride_y);
                        let dc15 = Bink2DSP::calc_dc(&src[24 + stride_y * 24..], stride_y);
                        self.y_dcs.set_dcs(bx, dc5, dc7, dc13, dc10, dc11, dc14, dc15);
                        let src = &dst[off_u + bx * 16..];
                        let dc1 = Bink2DSP::calc_dc(&src[8..], stride_u);
                        let dc2 = Bink2DSP::calc_dc(&src[0 + stride_u * 8..], stride_u);
                        let dc3 = Bink2DSP::calc_dc(&src[8 + stride_u * 8..], stride_u);
                        self.u_dcs.set_dcs(bx, dc1, dc2, dc3);
                        let src = &dst[off_v + bx * 16..];
                        let dc1 = Bink2DSP::calc_dc(&src[8..], stride_v);
                        let dc2 = Bink2DSP::calc_dc(&src[0 + stride_v * 8..], stride_v);
                        let dc3 = Bink2DSP::calc_dc(&src[8 + stride_v * 8..], stride_v);
                        self.v_dcs.set_dcs(bx, dc1, dc2, dc3);
                        if do_alpha {
                            let src = &dst[off_a + bx * 32..];
                            let dc5 = Bink2DSP::calc_dc(&src[24..], stride_a);
                            let dc7 = Bink2DSP::calc_dc(&src[24 + stride_y * 8..], stride_a);
                            let dc13 = Bink2DSP::calc_dc(&src[24 + stride_y * 16..], stride_a);
                            let dc10 = Bink2DSP::calc_dc(&src[ 0 + stride_y * 24..], stride_a);
                            let dc11 = Bink2DSP::calc_dc(&src[ 8 + stride_y * 24..], stride_a);
                            let dc14 = Bink2DSP::calc_dc(&src[16 + stride_y * 24..], stride_a);
                            let dc15 = Bink2DSP::calc_dc(&src[24 + stride_y * 24..], stride_a);
                            self.a_dcs.set_dcs(bx, dc5, dc7, dc13, dc10, dc11, dc14, dc15);
                        }
                    }
                }
                self.qinfo.update_line();
                self.y_dcs.update_line();
                self.u_dcs.update_line();
                self.v_dcs.update_line();
                self.a_dcs.update_line();
                self.mvs.update_line();
                off_y += stride_y * 32;
                off_u += stride_u * 16;
                off_v += stride_v * 16;
                off_a += stride_a * 32;
                row_state = (row_state & !0x190) | ((row_state & 4) << 2);
            }
            start_by = self.slice_h[slice_no];
        }
        Ok(())
    }
}

fn decode_flags(br: &mut BitReader, dst: &mut Vec<bool>, start: usize, nbits: usize) -> DecoderResult<()> {
    if start > 0 {
        dst.push(false);
    }
    if !br.read_bool()? {
        for _ in 0..nbits {
            let bit                             = br.read_bool()?;
            dst.push(bit);
        }
    } else {
        let mut cur_bits = nbits;
        let mut mode = 0;
        let mut lastbit = false;
        while cur_bits > 0 {
            if !br.read_bool()? {
                lastbit = if mode == 3 { !lastbit } else { br.read_bool()? };
                dst.push(lastbit);
                cur_bits -= 1;
                let len = cur_bits.min(4);
                for _ in 0..len {
                    let bit                     = br.read_bool()?;
                    dst.push(bit);
                }
                cur_bits -= len;
                mode = 2;
            } else {
                let bread: u8;
                if cur_bits < 4 {
                    bread = 2;
                } else if cur_bits < 16 {
                    bread = 4;
                } else {
                    bread = 4 | 1;
                }
                lastbit = if mode == 3 { !lastbit } else { br.read_bool()? };
                let mut run = (if mode == 3 { bread + 1 } else { bread + 2 } as usize).min(cur_bits);
                if run != cur_bits {
                    let add_run = br.read(bread)? as usize;
                    run += add_run;
                    mode = if add_run == (1 << bread) - 1 { 1 } else { 3 };
                }
                for _ in 0..run {
                    dst.push(lastbit);
                }
                cur_bits -= run;
            }
        }
    }
    Ok(())
}

fn get_new_quant(br: &mut BitReader, prev_q: u8) -> DecoderResult<u8> {
    let mut code                                = br.read_code(UintCodeType::LimitedUnary(4, 1))? as u8;
    if code == 0 {
        Ok(prev_q)
    } else {
        if code == 3 {
            code                                += br.read(1)? as u8;
        } else if code == 4 {
            code                                = (br.read(5)? as u8) + 5;
        }
        let ret = if !br.read_bool()? {
                prev_q.checked_add(code)
            } else {
                prev_q.checked_sub(code)
            };
        validate!(ret.is_some());
        let val = ret.unwrap();
        validate!(val < (BINK2_QUANT_DC.len() as u8));
        Ok(val)
    }
}

fn decode_luma_intra(br: &mut BitReader, codes: &Bink2Codes, prev_cbp: u32, q: u8, dst: &mut [[[i32; 64]; 4]; 4], edge_state: u32, dcinfo: &mut YDCInfo, bx: usize) -> DecoderResult<u32> {
    let cbp = decode_cbp_luma(br, prev_cbp, edge_state)?;
    let _coded = dcinfo.decode_dcs(br, q)?;
    dcinfo.predict(bx, (edge_state & 0x88) != 0, (edge_state & 0x20) != 0, 0, 0x7FF);
    let dcs = &dcinfo.dcs;
    for i in 0..4 {
        decode_acs_4blocks(br, codes, &mut dst[i], BINK2_QUANT_INTRA_LUMA, q, cbp >> (i * 4))?;
        for j in 0..4 { dst[i][j][0] = dcs[i * 4 + j] * 8 + 32; }
    }
    Ok(cbp)
}

fn decode_chroma_intra(br: &mut BitReader, codes: &Bink2Codes, prev_cbp: u32, q: u8, dst: &mut [[i32; 64]; 4], edge_state: u32, dcinfo: &mut CDCInfo, bx: usize) -> DecoderResult<u32> {
    let cbp = decode_cbp_chroma(br, prev_cbp)?;
    let _coded = dcinfo.decode_dcs(br, q)?;
    dcinfo.predict(bx, (edge_state & 0x88) != 0, (edge_state & 0x20) != 0, 0, 0x7FF);
    decode_acs_4blocks(br, codes, dst, BINK2_QUANT_INTRA_CHROMA, q, cbp)?;
    let dcs = &dcinfo.dcs;
    for i in 0..4 { dst[i][0] = dcs[i] * 8 + 32; }
    Ok(cbp)
}

// kb2i: KB2i adds the +32 rounding term to the inter DC coefficient as well (like intra); without it every inter
// block floors its residual (-0.5 per frame) and the picture drifts dark/green over a few dozen P frames.
fn decode_luma_inter(br: &mut BitReader, codes: &Bink2Codes, prev_cbp: u32, q: u8, dst: &mut [[[i32; 64]; 4]; 4], edge_state: u32, dcinfo: &mut YDCInfo, kb2i: bool) -> DecoderResult<u32> {
    let dc_bias = if kb2i { 32 } else { 0 };
    let cbp = decode_cbp_luma(br, prev_cbp, edge_state)?;
    let (min_dc, max_dc) = if (edge_state & 0x40000) != 0 { (-0x7FF, 0x7FF) } else { (-0x3FF, 0x3FF) };
    let _coded = dcinfo.decode_dcs(br, q)?;
    dcinfo.predict_inter(min_dc, max_dc);
    let dcs = &dcinfo.dcs;
    for i in 0..4 {
        decode_acs_4blocks(br, codes, &mut dst[i], BINK2_QUANT_INTER, q, cbp >> (i * 4))?;
        for j in 0..4 { dst[i][j][0] = dcs[i * 4 + j] * 8 + dc_bias; }
    }
    Ok(cbp)
}

fn decode_chroma_inter(br: &mut BitReader, codes: &Bink2Codes, prev_cbp: u32, q: u8, dst: &mut [[i32; 64]; 4], edge_state: u32, dcinfo: &mut CDCInfo, kb2i: bool) -> DecoderResult<u32> {
    let dc_bias = if kb2i { 32 } else { 0 };
    let cbp = decode_cbp_chroma(br, prev_cbp)?;
    let (min_dc, max_dc) = if (edge_state & 0x40000) != 0 { (-0x7FF, 0x7FF) } else { (-0x3FF, 0x3FF) };
    let _coded = dcinfo.decode_dcs(br, q)?;
    dcinfo.predict_inter(min_dc, max_dc);
    let dcs = &dcinfo.dcs;
    decode_acs_4blocks(br, codes, dst, BINK2_QUANT_INTER, q, cbp)?;
    for i in 0..4 { dst[i][0] = dcs[i] * 8 + dc_bias; }
    Ok(cbp)
}

#[allow(clippy::collapsible_if)]
fn decode_cbp_luma(br: &mut BitReader, prev_cbp: u32, edge_state: u32) -> DecoderResult<u32> {
    let cnt = (prev_cbp as u16).count_ones() as usize;
    let (min_count, mask) = if cnt < 8 { (cnt, 0x0000) } else { (16 - cnt, 0xFFFF) };

    let mut cbp;
    if br.read_bool()? {
        cbp = 0;
    } else if min_count >= 4 {
        cbp                                     = br.read(16)?;
    } else {
        cbp = 0;
        for i in 0..4 {
            if !br.read_bool()? {
                let cbp4                        = br.read(4)?;
                cbp |= cbp4 << (i * 4);
            }
        }
    }
    cbp ^= mask;
    if ((edge_state & 0x40000) == 0) || (cbp != 0) {
        if br.read_bool()? {
            cbp |= cbp << 16;
        }
    }
    Ok(cbp)
}
fn decode_cbp_chroma(br: &mut BitReader, last_cbp: u32) -> DecoderResult<u32> {
    if br.read_bool()? {
        Ok((last_cbp & 0xF0000) | BINK2_CHROMA_CBPS[(last_cbp & 0xF) as usize])
    } else {
        let ncbp                                = br.read(4)?;
        if br.read_bool()? {
            Ok(ncbp | (ncbp << 16))
        } else {
            Ok(ncbp)
        }
    }
}

trait ReadBink2Code {
    fn read_bink2_code_zero(&mut self) -> DecoderResult<i32>;
    fn read_bink2_code_nz(&mut self) -> DecoderResult<i32>;
}

impl<'a> ReadBink2Code for BitReader<'a> {
    fn read_bink2_code_zero(&mut self) -> DecoderResult<i32> {
        let pfx = self.read_code(UintCodeType::LimitedUnary(12, 0))? as i32;
        if pfx < 4 {
            Ok(pfx)
        } else {
            let add_bits                = self.read((pfx as u8) - 3)? as i32;
            Ok((1 << (pfx - 3)) + add_bits + 2)
        }
    }
    fn read_bink2_code_nz(&mut self) -> DecoderResult<i32> {
        let pfx = self.read_code(UintCodeType::LimitedUnary(12, 0))? + 1;
        let val = if pfx >= 4 {
                let add_bits            = self.read((pfx as u8) - 3)? as i32;
                (1 << (pfx - 3)) + add_bits + 2
            } else {
                pfx as i32
            };
        if self.read_bool()? {
            Ok(-val)
        } else {
            Ok(val)
        }
    }
}

fn decode_acs_4blocks(br: &mut BitReader, codes: &Bink2Codes, dst: &mut [[i32; 64]; 4], quant_mat: &[[i32; 64]; 4], q: u8, cbp: u32) -> DecoderResult<()> {
    let cb = if (cbp >> 16) != 0 { &codes.ac_cb1 } else { &codes.ac_cb2 };
    let qmat = &quant_mat[(q & 3) as usize];
    let shift = q >> 2;
    for (blk_no, block) in dst.iter_mut().enumerate() {
        if ((cbp >> blk_no) & 1) == 0 { continue; }
        let mut esc_len = 0;
        let mut idx = 1;
        while idx < 64 {
            if esc_len >= 1 {
                esc_len -= 1;
            } else {
                let sym                         = br.read_cb(cb)?;
                let skip = if sym == 11 {
                                                  br.read(6)? as usize
                    } else {
                        BINK2_AC_RUNS[sym as usize]
                    };
                idx += skip;
                if idx > 63 { break; }
                esc_len = if sym == 13 { 6 } else { 0 };
            }
            let level                           = br.read_bink2_code_nz()?;
            let pos = BINK2_ZIGZAG[idx];
            block[pos] = (((level * qmat[pos]) << shift) + 0x40) >> 7;
            idx += 1;
        }
// set blk info to (dc & 0x1FFF) | (intra ? 0x8000 : 0) | (((ncoded == 0) + (ncoded < 4) + (ncoded < 8)) << 13)
    }
    Ok(())
}

// --- KB2d (WWE '13 Bink 2 library, BINK section 0x8334F000; reverse-engineered 2026-11-04 for this converter) ---
// Macroblock: q = 4 raw bits (no CBP, no delta quant), then the old DC stage, then 4 coefficient groups (0x83364328).
// Coefficient group (4 blocks): position 1..63 in scan order, split into bands (end positions in KB2D_BANDS_*). Per band:
// 1 bit "rest zero" (stop), else width w = 3 bits (+2 if 7, i.e. 0..10). Width 0: band is zero. Otherwise per position:
// four w-bit magnitudes (block 0 first, LSB-first), then for each block in order with magnitude != 0 one sign bit
// (1 = negative). Dequantisation as the old path (the dequant/IDCT function is byte-identical between KB2d and KB2f).
const KB2D_BANDS_LUMA_INTRA: [usize; 8] = [ 2, 4, 8, 13, 19, 24, 41, 63 ];
const KB2D_BANDS_LUMA_INTER: [usize; 8] = [ 2, 4, 8, 13, 19, 24, 44, 63 ];
const KB2D_BANDS_CHROMA: [usize; 6] = [ 2, 5, 7, 9, 12, 63 ];

fn decode_acs_4blocks_kb2d(br: &mut BitReader, dst: &mut [[f32; 64]; 4], quant_mat: &[f32; 64], q: u8, bands: &[usize], scan: &[usize; 64]) -> DecoderResult<()> {
    let quant = BINK2_OLD_QUANTS[q as usize];
    let mut idx = 1;
    for &band_end in bands.iter() {
        if idx >= 64 { break; }
        if br.read_bool()? { break; } // rest of the coefficients are zero
        let mut w = br.read(3)? as u8;
        if w == 7 { w += br.read(2)? as u8; }
        while idx <= band_end && idx < 64 {
            if w > 0 {
                let mut mags = [0u32; 4];
                for m in mags.iter_mut() { *m = br.read(w)?; }
                let pos = scan[idx];
                for (blk, &m) in mags.iter().enumerate() {
                    if m == 0 { continue; }
                    let mut level = m as i32;
                    if br.read_bool()? { level = -level; }
                    dst[blk][pos] = (level as f32) * quant_mat[(pos & 7) * 8 + (pos >> 3)] * quant;
                }
            }
            idx += 1;
        }
    }
    Ok(())
}

fn decode_luma_intra_kb2d(br: &mut BitReader, dst: &mut [[[f32; 64]; 4]; 4], edge_state: u32, dcinfo: &mut YDCInfo, bx: usize) -> DecoderResult<()> {
    let q = br.read(4)? as u8;
    let is_top = (edge_state & 0x88) != 0;
    let is_left = (edge_state & 0x20) != 0;
    decode_dcs_old(br, &mut dcinfo.dc_buf, q, is_top && is_left)?;
    dcinfo.predict(bx, is_top, is_left, 0, 0x7FF);
    let dcs = &dcinfo.dcs;
    for i in 0..4 {
        decode_acs_4blocks_kb2d(br, &mut dst[i], BINK2_QMAT_INTRA_LUMA_OLD, q, &KB2D_BANDS_LUMA_INTRA, BINK2_LUMA_SCAN_OLD)?;
        for j in 0..4 { dst[i][j][0] = (dcs[i * 4 + j] as f32) * 0.125; }
    }
    Ok(())
}

fn decode_chroma_intra_kb2d(br: &mut BitReader, dst: &mut [[f32; 64]; 4], edge_state: u32, dcinfo: &mut CDCInfo, bx: usize) -> DecoderResult<()> {
    let q = br.read(4)? as u8;
    let is_top = (edge_state & 0x88) != 0;
    let is_left = (edge_state & 0x20) != 0;
    decode_dcs_old(br, &mut dcinfo.dc_buf, q, is_top && is_left)?;
    dcinfo.predict(bx, is_top, is_left, 0, 0x7FF);
    decode_acs_4blocks_kb2d(br, dst, BINK2_QMAT_CHROMA_OLD, q, &KB2D_BANDS_CHROMA, BINK2_CHROMA_SCAN_OLD)?;
    let dcs = &dcinfo.dcs;
    for i in 0..4 { dst[i][0] = (dcs[i] as f32) * 0.125; }
    Ok(())
}

fn decode_luma_inter_kb2d(br: &mut BitReader, dst: &mut [[[f32; 64]; 4]; 4], dcinfo: &mut YDCInfo) -> DecoderResult<()> {
    let q = br.read(4)? as u8;
    decode_dcs_old(br, &mut dcinfo.dc_buf, q, false)?;
    dcinfo.predict_inter(-1023, 1023);
    let dcs = &dcinfo.dcs;
    for i in 0..4 {
        decode_acs_4blocks_kb2d(br, &mut dst[i], BINK2_QMAT_INTER_LUMA_OLD, q, &KB2D_BANDS_LUMA_INTER, BINK2_LUMA_SCAN_OLD)?;
        for j in 0..4 { dst[i][j][0] = (dcs[i * 4 + j] as f32) * 0.125; }
    }
    Ok(())
}

// KB2d inter chroma (0x8336C550): ONE call for both planes: q = 4 bits, one DC set of 8 values (0x833693C8: shared
// bit width, two chunks of 4, no bias in inter), then coefficients of the first plane, then of the second.
fn decode_chroma_inter_kb2d(br: &mut BitReader, dst1: &mut [[f32; 64]; 4], dst2: &mut [[f32; 64]; 4], dc1: &mut CDCInfo, dc2: &mut CDCInfo) -> DecoderResult<()> {
    let q = br.read(4)? as u8;
    let mut dcs8 = [0i32; 8];
    decode_dcs_old(br, &mut dcs8, q, false)?;
    dc1.dc_buf.copy_from_slice(&dcs8[..4]);
    dc2.dc_buf.copy_from_slice(&dcs8[4..]);
    dc1.predict_inter(-1023, 1023);
    dc2.predict_inter(-1023, 1023);
    decode_acs_4blocks_kb2d(br, dst1, BINK2_QMAT_CHROMA_OLD, q, &KB2D_BANDS_CHROMA, BINK2_CHROMA_SCAN_OLD)?;
    decode_acs_4blocks_kb2d(br, dst2, BINK2_QMAT_CHROMA_OLD, q, &KB2D_BANDS_CHROMA, BINK2_CHROMA_SCAN_OLD)?;
    for i in 0..4 { dst1[i][0] = (dc1.dcs[i] as f32) * 0.125; }
    for i in 0..4 { dst2[i][0] = (dc2.dcs[i] as f32) * 0.125; }
    Ok(())
}

fn decode_luma_intra_old(br: &mut BitReader, codes: &Bink2Codes, prev_cbp: u32, dst: &mut [[[f32; 64]; 4]; 4], edge_state: u32, dcinfo: &mut YDCInfo, bx: usize, old_q: &mut u8) -> DecoderResult<u32> {
    let cbp = decode_cbp_luma_old(br, prev_cbp)?;
    let q = decode_quant_old(br, &codes.quant_cb, *old_q)?;
    *old_q = q;
    let is_top = (edge_state & 0x88) != 0;
    let is_left = (edge_state & 0x20) != 0;
    decode_dcs_old(br, &mut dcinfo.dc_buf, q, is_top && is_left)?;
    dcinfo.predict(bx, is_top, is_left, 0, 0x7FF);
    let dcs = &dcinfo.dcs;
    for i in 0..4 {
        decode_acs_4blocks_old(br, codes, &mut dst[i], BINK2_QMAT_INTRA_LUMA_OLD, q, cbp >> (i * 4), BINK2_LUMA_SCAN_OLD)?;
        for j in 0..4 { dst[i][j][0] = (dcs[i * 4 + j] as f32) * 0.125; }
    }
    Ok(cbp)
}

fn decode_chroma_intra_old(br: &mut BitReader, codes: &Bink2Codes, prev_cbp: u32, dst: &mut [[f32; 64]; 4], edge_state: u32, dcinfo: &mut CDCInfo, bx: usize, old_q: &mut u8) -> DecoderResult<u32> {
    let cbp = decode_cbp_chroma_old(br, prev_cbp)?;
    let q = decode_quant_old(br, &codes.quant_cb, *old_q)?;
    *old_q = q;
    let is_top = (edge_state & 0x88) != 0;
    let is_left = (edge_state & 0x20) != 0;
    decode_dcs_old(br, &mut dcinfo.dc_buf, q, is_top && is_left)?;
    dcinfo.predict(bx, is_top, is_left, 0, 0x7FF);
    decode_acs_4blocks_old(br, codes, dst, BINK2_QMAT_CHROMA_OLD, q, cbp, BINK2_CHROMA_SCAN_OLD)?;
    let dcs = &dcinfo.dcs;
    for i in 0..4 { dst[i][0] = (dcs[i] as f32) * 0.125; }
    Ok(cbp)
}


fn decode_luma_inter_old(br: &mut BitReader, codes: &Bink2Codes, prev_cbp: u32, dst: &mut [[[f32; 64]; 4]; 4], _edge_state: u32, dcinfo: &mut YDCInfo, old_q: &mut u8) -> DecoderResult<u32> {
    let cbp = decode_cbp_luma_old(br, prev_cbp)?;
    let q = decode_quant_old(br, &codes.quant_cb, *old_q)?;
    *old_q = q;
    decode_dcs_old(br, &mut dcinfo.dc_buf, q, false)?;
    dcinfo.predict_inter(-1023, 1023);
    let dcs = &dcinfo.dcs;
    for i in 0..4 {
        decode_acs_4blocks_old(br, codes, &mut dst[i], BINK2_QMAT_INTER_LUMA_OLD, q, cbp >> (i * 4), BINK2_LUMA_SCAN_OLD)?;
        for j in 0..4 { dst[i][j][0] = (dcs[i * 4 + j] as f32) * 0.125; }
    }
    Ok(cbp)
}

fn decode_chroma_inter_old(br: &mut BitReader, codes: &Bink2Codes, prev_cbp: u32, dst: &mut [[f32; 64]; 4], _edge_state: u32, dcinfo: &mut CDCInfo, old_q: &mut u8) -> DecoderResult<u32> {
    let cbp = decode_cbp_chroma_old(br, prev_cbp)?;
    let q = decode_quant_old(br, &codes.quant_cb, *old_q)?;
    *old_q = q;
    decode_dcs_old(br, &mut dcinfo.dc_buf, q, false)?;
    dcinfo.predict_inter(-1023, 1023);
    let dcs = &dcinfo.dcs;
    decode_acs_4blocks_old(br, codes, dst, BINK2_QMAT_CHROMA_OLD, q, cbp, BINK2_CHROMA_SCAN_OLD)?;
    for i in 0..4 { dst[i][0] = (dcs[i] as f32) * 0.125; }
    Ok(cbp)
}

fn decode_quant_old(br: &mut BitReader, quant_cb: &Codebook<u8>, prev_quant: u8) -> DecoderResult<u8> {
    let diff                                    = br.read_cb(quant_cb)?;
    if diff == 0 {
        Ok(prev_quant)
    } else {
        let res = if br.read_bool()? {
                prev_quant.checked_sub(diff)
            } else {
                prev_quant.checked_add(diff)
            };
        validate!(res.is_some());
        let q = res.unwrap();
        validate!(q < 16);
        Ok(q)
    }
}

fn decode_cbp_luma_old(br: &mut BitReader, prev_cbp: u32) -> DecoderResult<u32> {
    let low;
    if br.read_bool()? {
        if br.read_bool()? {
            return Ok(prev_cbp);
        }
        low = prev_cbp & 0xFFFF;
    } else {
        let mut nib1 = (prev_cbp >> 4) & 0xF;
        if !br.read_bool()? {
            nib1                                = br.read(4)?;
        }
        let mut new_cbp = nib1;
        if !br.read_bool()? {
            nib1                                = br.read(4)?;
        }
        new_cbp |= nib1 << 4;
        if !br.read_bool()? {
            nib1                                = br.read(4)?;
        }
        new_cbp |= nib1 << 8;
        if !br.read_bool()? {
            nib1                                = br.read(4)?;
        }
        low = new_cbp | (nib1 << 12);
    }
    let mut nib_hi = (prev_cbp >> 20) & 0xF;
    let mut high = 0;
    for i in 0..4 {
        let nib = (low >> (4 * i)) & 0xF;
        if nib.count_ones() == 0 {
            nib_hi = 0;
        } else if (nib.count_ones() == 1) || !br.read_bool()? {
            nib_hi = 0;
            for bit in 0..4 {
                if (((nib >> bit) & 1) != 0) && br.read_bool()? {
                    nib_hi |= 1 << bit;
                }
            }
        }
        nib_hi &= nib;
        high = (high >> 4) | (nib_hi << 28);
    }
    Ok(low | high)
}

fn decode_cbp_chroma_old(br: &mut BitReader, prev_cbp: u32) -> DecoderResult<u32> {
    let low = if !br.read_bool()? {
                                                  br.read(4)?
        } else {
            if br.read_bool()? {
                return Ok(prev_cbp);
            }
            prev_cbp & 0xF
        };
    let mut high;
    if low.count_ones() == 0 {
        return Ok(low);
    }
    if low.count_ones() != 1 {
        high = (prev_cbp >> 16) & low;
        if br.read_bool()? {
            return Ok((high << 16) | low);
        }
    }
    high = 0;
    for bit in 0..4 {
        if (((low >> bit) & 1) != 0) && br.read_bool()? {
            high |= 1 << bit;
        }
    }
    Ok((high << 16) | low)
}

fn decode_dcs_old(br: &mut BitReader, dcs: &mut [i32], q: u8, read_bias: bool) -> DecoderResult<()> {
    let mut bits                                = br.read(3)? as u8;
    if bits == 7 {
        bits                                    += br.read(2)? as u8;
    }
    let scale = BINK2_OLD_DC_QUANTS[q as usize];
    if bits == 0 {
        for el in dcs.iter_mut() { *el = 0; }
    } else {
        for dcb in dcs.chunks_mut(4) {
            for dc in dcb.iter_mut() {
                *dc                             = br.read(bits)? as i32;
            }
            for dc in dcb.iter_mut() {
                if *dc == 0 { continue; }
                *dc *= scale;
                if br.read_bool()? {
                    *dc = -*dc;
                }
            }
        }
    }
    if read_bias {
        let add_bits = BINK2_DC_EXTRA_BITS[q as usize] + bits;
        if add_bits < 10 {
            let pfx                             = br.read(10 - add_bits)? as i32;
            let base_dc;
            if pfx > 0 {
                if br.read_bool()? {
                    base_dc = -(pfx << bits);
                } else {
                    base_dc = pfx << bits;
                }
            } else {
                base_dc = 0;
            }
            dcs[0] += base_dc * scale;
        }
    }
    Ok(())
}

fn decode_acs_4blocks_old(br: &mut BitReader, codes: &Bink2Codes, dst: &mut [[f32; 64]; 4], quant_mat: &[f32; 64], q: u8, cbp: u32, scan: &[usize; 64]) -> DecoderResult<()> {
    let quant = BINK2_OLD_QUANTS[q as usize];
    for (blk_no, block) in dst.iter_mut().enumerate() {
        if ((cbp >> blk_no) & 1) == 0 { continue; }
        let val_cb = if ((cbp >> (16 + blk_no)) & 1) != 0 { &codes.val_cb2 } else { &codes.val_cb1 };
        let skip_cb = if ((cbp >> (16 + blk_no)) & 1) != 0 { &codes.skip_cb2 } else { &codes.skip_cb1 };
        let mut esc_len = 0;
        let mut idx = 1;
        while idx < 64 {
            let val                             = br.read_cb(val_cb)?;
            if val != 0 {
                let mut level;
                if val >= 4 {
                    let add_bits                = br.read(val - 3)? as i32;
                    level = (1 << (val - 3)) + add_bits + 2;
                } else {
                    level = val as i32;
                }
                if br.read_bool()? {
                    level = -level;
                }
                let pos = scan[idx];
                block[pos] = (level as f32) * quant_mat[(pos & 7) * 8 + (pos >> 3)] * quant;
            }
            idx += 1;
            if idx >= 64 { break; }
            if esc_len >= 1 {
                esc_len -= 1;
            } else {
                let sym                         = br.read_cb(skip_cb)?;
                let skip = if sym == 11 {
                                                  br.read(6)? as usize
                    } else {
                        BINK2_AC_RUNS[sym as usize]
                    };
                idx += skip;
                if idx > 63 { break; }
                esc_len = if sym == 13 { 6 } else { 0 };
            }
        }
    }
    Ok(())
}

const KB2H_NUM_SLICES: [usize; 4] = [ 2, 3, 4, 8 ];

impl NADecoder for Bink2Decoder {
    fn init(&mut self, _supp: &mut NADecoderSupport, info: NACodecInfoRef) -> DecoderResult<()> {
        if let NACodecTypeInfo::Video(vinfo) = info.get_properties() {
            let w = vinfo.get_width();
            let h = vinfo.get_height();

            let edata = info.get_extradata().unwrap();
            validate!(edata.len() >= 8);

            let mut br = MemoryReader::new_read(&edata);
            let magic                   = br.read_u32be()?;
            let flags                   = br.read_u32le()?;

            self.version = magic;
            self.is_kb2d = magic == mktag!(b"KB2d");
            self.is_kb2i = magic >= mktag!(b"KB2i");
            self.has_alpha = (flags & 0x100000) != 0;

            let height_a = (h + 31) & !31;
            if self.version <= mktag!(b"KB2f") {
                self.num_slices = 2;
                self.slice_h[0] = (h + 32) >> 6;
            } else if self.version == mktag!(b"KB2g") {
                if height_a < 128 {
                    self.num_slices = 1;
                } else {
                    self.num_slices = 2;
                    self.slice_h[0] = (h + 31) >> 6;
                }
            } else {
                self.num_slices = KB2H_NUM_SLICES[(flags & 3) as usize];
                let mut start = 0;
                let mut end = height_a + 32 * self.num_slices - 1;
                for i in 0..self.num_slices - 1 {
                    start += ((end - start) / (self.num_slices - i)) & !31;
                    end -= 32;
                    self.slice_h[i] = start >> 5;
                }
            }
            self.slice_h[self.num_slices - 1] = height_a >> 5;

            let aplane = if self.has_alpha { Some(NAPixelChromaton::new(0, 0, false, 8, 0, 3, 1)) } else { None };
            let fmt = NAPixelFormaton::new(ColourModel::YUV(YUVSubmodel::YUVJ),
                                           Some(NAPixelChromaton::new(0, 0, false, 8, 0, 0, 1)),
                                           Some(NAPixelChromaton::new(1, 1, false, 8, 0, 1, 1)),
                                           Some(NAPixelChromaton::new(1, 1, false, 8, 0, 2, 1)),
                                           aplane, None,
                                           if self.has_alpha { FORMATON_FLAG_ALPHA } else { 0 },
                                           if self.has_alpha { 4 } else { 3 });
            let myinfo = NACodecTypeInfo::Video(NAVideoInfo::new(w, h, false, fmt));
            self.info = NACodecInfo::new_ref(info.get_name(), myinfo, info.get_extradata()).into_ref();

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

        let bufinfo = alloc_video_buffer(self.info.get_properties().get_video_info().unwrap(), 5)?;
        buf = bufinfo.get_vbuf().unwrap();

        self.decode_frame_new(&mut br, &mut buf, pkt.is_keyframe())?;
        let bufinfo = NABufferType::Video(buf);
        self.ips.add_frame(bufinfo.get_vbuf().unwrap());

        let mut frm = NAFrame::new_from_pkt(pkt, self.info.clone(), bufinfo);
        frm.set_frame_type(if self.key_frame { FrameType::I } else { FrameType::P });
        Ok(frm.into_ref())
    }
    fn flush(&mut self) {
        self.ips.clear();
    }
}

impl NAOptionHandler for Bink2Decoder {
    fn get_supported_options(&self) -> &[NAOptionDefinition] { &[] }
    fn set_options(&mut self, _options: &[NAOption]) { }
    fn query_option_value(&self, _name: &str) -> Option<NAValue> { None }
}

pub fn get_decoder() -> Box<dyn NADecoder + Send> {
    Box::new(Bink2Decoder::new())
}

#[cfg(test)]
mod test {
    use nihav_core::codecs::RegisteredDecoders;
    use nihav_core::demuxers::RegisteredDemuxers;
    use nihav_codec_support::test::dec_video::*;
    use crate::rad_register_all_decoders;
    use crate::rad_register_all_demuxers;
    #[test]
    fn test_bink2() {
        let mut dmx_reg = RegisteredDemuxers::new();
        rad_register_all_demuxers(&mut dmx_reg);
        let mut dec_reg = RegisteredDecoders::new();
        rad_register_all_decoders(&mut dec_reg);

        //let file = "assets/RAD/Open_Logos_partial.bik";
        //let file = "assets/RAD/sc13_01_partial.bk2";
        // sample from a private collection
        let file = "assets/RAD/ge_video_86l.bk2";
        //let file = "assets/RAD/eg_club_0.bk2";
        test_file_decoding("bink", file, Some(8), true, false, None/*Some("bink2")*/, &dmx_reg, &dec_reg);
    }
}

const BINK2_ZIGZAG: [usize; 64] = [
     0,  8,  1,  2,  9, 16, 24, 17,
    10,  3,  4, 11, 18, 25, 32, 40,
    33, 26, 19, 12,  5,  6, 13, 20,
    27, 34, 41, 48, 56, 49, 42, 35,
    28, 21, 14,  7, 15, 22, 29, 36,
    43, 50, 57, 58, 51, 44, 37, 30,
    23, 31, 38, 45, 52, 59, 60, 53,
    46, 39, 47, 54, 61, 62, 55, 63
];
const BINK2_LUMA_SCAN_OLD: &[usize; 64] = &[
     0,  2,  1,  8,  9, 17, 10, 16,
    24,  3, 18, 25, 32, 11, 33, 26,
     4, 40, 19, 12, 27, 41, 34,  5,
    20, 48,  6, 28, 15, 42, 23, 35,
    21, 13, 14,  7, 31, 43, 49, 36,
    22, 56, 39, 50, 30, 44, 29, 51,
    57, 47, 58, 59, 63, 61, 55, 38,
    52, 62, 45, 37, 60, 46, 54, 53
];
const BINK2_CHROMA_SCAN_OLD: &[usize; 64] = &[
     0,  1,  8,  2,  9, 16, 10, 17,
     3, 24, 11, 18, 25, 13, 14,  4,
    15,  5,  6,  7, 12, 19, 20, 21,
    22, 23, 26, 27, 28, 29, 30, 31,
    32, 33, 34, 35, 36, 37, 38, 39,
    40, 41, 42, 43, 44, 45, 46, 47,
    48, 49, 50, 51, 52, 53, 54, 55,
    56, 57, 58, 59, 60, 61, 62, 63
];

const BINK2_CHROMA_CBPS: [u32; 16] = [
    0x0, 0x0, 0x0, 0xF,
    0x0, 0xF, 0xF, 0xF,
    0x0, 0xF, 0xF, 0xF,
    0xF, 0xF, 0xF, 0xF
];

const BINK2_QUANT_DC: [i32; 37] = [
    0x400,   0x4C2,   0x5A8,   0x6BA,   0x800,   0x983,   0xB50,   0xD74,
    0x1000,  0x1307,  0x16A1,  0x1AE9,  0x2000,  0x260E,  0x2D41,  0x35D1,
    0x4000,  0x4C1C,  0x5A82,  0x6BA3,  0x8000,  0x9838,  0xB505,  0xD745,
    0x10000, 0x13070, 0x16A0A, 0x1AE8A, 0x20000, 0x260E0, 0x2D414, 0x35D14,
    0x40000, 0x4C1C0, 0x5A828, 0x6BA28, 0x80000
];

const BINK2_QUANT_INTRA_LUMA: &[[i32; 64]; 4] = &[
  [
     0x400,  0x598,  0x5E2,  0x49D,  0x733,  0x7E9, 0x1497, 0x2190,
     0x521,  0x685,  0x65E,  0x688,  0xA41,  0xD72, 0x1F57, 0x31FA,
     0x434,  0x6DB,  0x710,  0x79E,  0xF8C, 0x130B, 0x226D, 0x2E85,
     0x546,  0x74C,  0x84F,  0x7E0, 0x1275, 0x1162, 0x1E20, 0x25A5,
     0x99A,  0xC1F, 0x10CF, 0x10CF, 0x1B33, 0x1AB3, 0x2B47, 0x3435,
     0xD2F, 0x1648, 0x13BC, 0x17A1, 0x23EE, 0x1C40, 0x29E5, 0x266A,
    0x156E, 0x1D61, 0x1E75, 0x1BB3, 0x2B47, 0x2720, 0x34F7, 0x3270,
    0x1C6F, 0x1DE1, 0x1B6C, 0x17D1, 0x23E5, 0x2357, 0x3175, 0x35C8
  ], [
     0x4C2,  0x6A7,  0x6FF,  0x57D,  0x890,  0x968, 0x187C, 0x27EA,
     0x619,  0x7C1,  0x792,  0x7C4,  0xC32,  0xFFD, 0x2545, 0x3B6F,
     0x4FF,  0x827,  0x866,  0x90F, 0x127D, 0x16A6, 0x28F1, 0x3752,
     0x646,  0x8AE,  0x9E2,  0x95E, 0x15F3, 0x14AC, 0x23D3, 0x2CC4,
     0xB6B,  0xE6A, 0x13FD, 0x13FE, 0x2059, 0x1FC0, 0x3378, 0x3E16,
     0xFAE, 0x1A7F, 0x1778, 0x1C1A, 0x2ABA, 0x2198, 0x31D3, 0x2DAE,
    0x197C, 0x22F0, 0x2438, 0x20F1, 0x3378, 0x2E87, 0x3EFD, 0x3BFA,
    0x21D1, 0x2388, 0x209C, 0x1C52, 0x2AAF, 0x2A07, 0x3AD0, 0x3FF5
  ], [
     0x5A8,  0x7E9,  0x852,  0x687,  0xA2F,  0xB30, 0x1D1E, 0x2F77,
     0x740,  0x938,  0x901,  0x93C,  0xE81, 0x1303, 0x2C52, 0x46AE,
     0x5F1,  0x9B2,  0x9FD,  0xAC6, 0x15FC, 0x1AEE, 0x30B0, 0x41C9,
     0x775,  0xA52,  0xBC0,  0xB24, 0x1A1B, 0x1895, 0x2A9A, 0x353D,
     0xD94, 0x1124, 0x17C5, 0x17C6, 0x2677, 0x25C2, 0x3D34, 0x49D5,
    0x12A5, 0x1F82, 0x1BE9, 0x216B, 0x32CF, 0x27F3, 0x3B40, 0x3653,
    0x1E4E, 0x298D, 0x2B12, 0x272C, 0x3D34, 0x3755, 0x4AE7, 0x4753,
    0x2836, 0x2A41, 0x26C7, 0x21AE, 0x32C3, 0x31FA, 0x45F1, 0x4C0E
  ], [
     0x6BA,  0x968,  0x9E5,  0x7C3,  0xC1C,  0xD4E, 0x22A0, 0x3872,
     0x8A0,  0xAF7,  0xAB5,  0xAFB, 0x113F, 0x169C, 0x34B5, 0x540D,
     0x711,  0xB87,  0xBE1,  0xCD0, 0x1A25, 0x2007, 0x39E6, 0x4E3C,
     0x8DF,  0xC46,  0xDFA,  0xD3F, 0x1F0B, 0x1D3C, 0x32A9, 0x3F4F,
    0x1025, 0x1462, 0x1C44, 0x1C46, 0x2DBF, 0x2CE7, 0x48C9, 0x57CE,
    0x162D, 0x2578, 0x2131, 0x27BE, 0x3C6D, 0x2F82, 0x4676, 0x409A,
    0x240A, 0x3169, 0x3338, 0x2E96, 0x48C9, 0x41CD, 0x5914, 0x54D3,
    0x2FD2, 0x3240, 0x2E1E, 0x280E, 0x3C5E, 0x3B6F, 0x532D, 0x5A73
  ]
];

const BINK2_QUANT_INTRA_CHROMA: &[[i32; 64]; 4] = &[
  [
     0x400,  0x4A9,  0x59A,  0x89B, 0x1600, 0x1221, 0x171C, 0x19A3,
     0x4A9,  0x656,  0x713,  0xE16, 0x19A3, 0x1520, 0x1AEE, 0x1DE1,
     0x59A,  0x713,  0xDBB, 0x130B, 0x171C, 0x130B, 0x1847, 0x1AEE,
     0x89B,  0xE16, 0x130B,  0xEF0, 0x1221,  0xEF0, 0x130B, 0x1520,
    0x1600, 0x19A3, 0x171C, 0x1221, 0x1600, 0x1221, 0x171C, 0x19A3,
    0x1221, 0x1520, 0x130B,  0xEF0, 0x1221,  0xEF0, 0x130B, 0x1520,
    0x171C, 0x1AEE, 0x1847, 0x130B, 0x171C, 0x130B, 0x1847, 0x1AEE,
    0x19A3, 0x1DE1, 0x1AEE, 0x1520, 0x19A3, 0x1520, 0x1AEE, 0x1DE1
  ], [
     0x4C2,  0x58B,  0x6AA,  0xA3C, 0x1A2A, 0x158F, 0x1B7B, 0x1E7D,
     0x58B,  0x789,  0x869, 0x10C0, 0x1E7D, 0x1920, 0x2007, 0x2388,
     0x6AA,  0x869, 0x1054, 0x16A6, 0x1B7B, 0x16A6, 0x1CDE, 0x2007,
     0xA3C, 0x10C0, 0x16A6, 0x11C4, 0x158F, 0x11C4, 0x16A6, 0x1920,
    0x1A2A, 0x1E7D, 0x1B7B, 0x158F, 0x1A2A, 0x158F, 0x1B7B, 0x1E7D,
    0x158F, 0x1920, 0x16A6, 0x11C4, 0x158F, 0x11C4, 0x16A6, 0x1920,
    0x1B7B, 0x2007, 0x1CDE, 0x16A6, 0x1B7B, 0x16A6, 0x1CDE, 0x2007,
    0x1E7D, 0x2388, 0x2007, 0x1920, 0x1E7D, 0x1920, 0x2007, 0x2388
  ], [
     0x5A8,  0x698,  0x7EC,  0xC2C, 0x1F1D, 0x19A3, 0x20AF, 0x2442,
     0x698,  0x8F6,  0xA01, 0x13EB, 0x2442, 0x1DE1, 0x2616, 0x2A41,
     0x7EC,  0xA01, 0x136B, 0x1AEE, 0x20AF, 0x1AEE, 0x2255, 0x2616,
     0xC2C, 0x13EB, 0x1AEE, 0x1520, 0x19A3, 0x1520, 0x1AEE, 0x1DE1,
    0x1F1D, 0x2442, 0x20AF, 0x19A3, 0x1F1D, 0x19A3, 0x20AF, 0x2442,
    0x19A3, 0x1DE1, 0x1AEE, 0x1520, 0x19A3, 0x1520, 0x1AEE, 0x1DE1,
    0x20AF, 0x2616, 0x2255, 0x1AEE, 0x20AF, 0x1AEE, 0x2255, 0x2616,
    0x2442, 0x2A41, 0x2616, 0x1DE1, 0x2442, 0x1DE1, 0x2616, 0x2A41
  ], [
     0x6BA,  0x7D7,  0x96C,  0xE7A, 0x2500, 0x1E7D, 0x26DE, 0x2B1E,
     0x7D7,  0xAA9,  0xBE5, 0x17B0, 0x2B1E, 0x2388, 0x2D4B, 0x3240,
     0x96C,  0xBE5, 0x1718, 0x2007, 0x26DE, 0x2007, 0x28D4, 0x2D4B,
     0xE7A, 0x17B0, 0x2007, 0x1920, 0x1E7D, 0x1920, 0x2007, 0x2388,
    0x2500, 0x2B1E, 0x26DE, 0x1E7D, 0x2500, 0x1E7D, 0x26DE, 0x2B1E,
    0x1E7D, 0x2388, 0x2007, 0x1920, 0x1E7D, 0x1920, 0x2007, 0x2388,
    0x26DE, 0x2D4B, 0x28D4, 0x2007, 0x26DE, 0x2007, 0x28D4, 0x2D4B,
    0x2B1E, 0x3240, 0x2D4B, 0x2388, 0x2B1E, 0x2388, 0x2D4B, 0x3240
  ]
];

const BINK2_QUANT_INTER: &[[i32; 64]; 4] = &[
  [
    0x400, 0x4A9, 0x434, 0x34C, 0x41C, 0x392, 0x4C9,  0x5D4,
    0x4A9, 0x56F, 0x4E6, 0x3D7, 0x4CB, 0x429, 0x5B7,  0x718,
    0x434, 0x4E6, 0x489, 0x3A8, 0x4AB, 0x40A, 0x5A4,  0x6CD,
    0x34C, 0x3D7, 0x3A8, 0x32B, 0x41F, 0x39F, 0x519,  0x630,
    0x41C, 0x4CB, 0x4AB, 0x41F, 0x5AB, 0x538, 0x778,  0x932,
    0x392, 0x429, 0x40A, 0x39F, 0x538, 0x521, 0x799,  0x9B6,
    0x4C9, 0x5B7, 0x5A4, 0x519, 0x778, 0x799, 0xBE4,  0xFC7,
    0x5D4, 0x718, 0x6CD, 0x630, 0x932, 0x9B6, 0xFC7, 0x162F
  ], [
    0x4C2, 0x58B, 0x4FF, 0x3EB, 0x4E4, 0x43F,  0x5B1,  0x6EE,
    0x58B, 0x676, 0x5D3, 0x491, 0x5B3, 0x4F3,  0x6CB,  0x86F,
    0x4FF, 0x5D3, 0x565, 0x459, 0x58D, 0x4CE,  0x6B5,  0x816,
    0x3EB, 0x491, 0x459, 0x3C5, 0x4E6, 0x44F,  0x610,  0x75C,
    0x4E4, 0x5B3, 0x58D, 0x4E6, 0x6BD, 0x635,  0x8E2,  0xAEF,
    0x43F, 0x4F3, 0x4CE, 0x44F, 0x635, 0x61A,  0x909,  0xB8C,
    0x5B1, 0x6CB, 0x6B5, 0x610, 0x8E2, 0x909,  0xE24, 0x12C3,
    0x6EE, 0x86F, 0x816, 0x75C, 0xAEF, 0xB8C, 0x12C3, 0x1A61
  ], [
    0x5A8, 0x698, 0x5F1, 0x4A9, 0x5D0, 0x50D,  0x6C4, 0x83E,
    0x698, 0x7AF, 0x6ED, 0x56F, 0x6C7, 0x5E3,  0x814, 0xA08,
    0x5F1, 0x6ED, 0x66A, 0x52B, 0x69A, 0x5B6,  0x7FA, 0x99E,
    0x4A9, 0x56F, 0x52B, 0x47B, 0x5D4, 0x51F,  0x735, 0x8C1,
    0x5D0, 0x6C7, 0x69A, 0x5D4, 0x804, 0x761,  0xA90, 0xD00,
    0x50D, 0x5E3, 0x5B6, 0x51F, 0x761, 0x741,  0xABF, 0xDBB,
    0x6C4, 0x814, 0x7FA, 0x735, 0xA90, 0xABF, 0x10D2, 0x1650,
    0x83E, 0xA08, 0x99E, 0x8C1, 0xD00, 0xDBB, 0x1650, 0x1F5F
  ], [
    0x6BA, 0x7D7, 0x711, 0x58B, 0x6EA,  0x601,  0x80C,  0x9CD,
    0x7D7, 0x923, 0x83C, 0x676, 0x80F,  0x700,  0x99C,  0xBEE,
    0x711, 0x83C, 0x7A1, 0x626, 0x7DA,  0x6CB,  0x97C,  0xB70,
    0x58B, 0x676, 0x626, 0x554, 0x6EE,  0x617,  0x893,  0xA68,
    0x6EA, 0x80F, 0x7DA, 0x6EE, 0x988,  0x8C7,  0xC90,  0xF76,
    0x601, 0x700, 0x6CB, 0x617, 0x8C7,  0x8A1,  0xCC7, 0x1055,
    0x80C, 0x99C, 0x97C, 0x893, 0xC90,  0xCC7, 0x1400, 0x1A89,
    0x9CD, 0xBEE, 0xB70, 0xA68, 0xF76, 0x1055, 0x1A89, 0x254E
  ]
];

const BINK2_AC_CODES1: &[u8] = &[
    0x01, 0x04, 0x00, 0x08, 0x02, 0x32, 0x0A, 0x12,
    0x3A, 0x7A, 0xFA, 0x72, 0x06, 0x1A
];
const BINK2_AC_BITS1: &[u8] = &[ 1, 3, 4, 4, 5, 7, 5, 6, 7, 8, 8, 7, 3, 6 ];
const BINK2_AC_CODES2: &[u16] = &[
    0x01,  0x00,  0x04,  0x2C, 0x6C, 0x0C, 0x4C, 0xAC,
    0xEC, 0x12C, 0x16C, 0x1AC, 0x02, 0x1C
];
const BINK2_AC_BITS2: &[u8] = &[ 1, 3, 4, 9, 9, 7, 7, 9, 8, 9, 9, 9, 2, 5 ];

const BINK2_AC_RUNS: [usize; 14] = [ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 64, 0 ];

const BINK2_MV_CODES: &[u8] = &[
    0x01, 0x06, 0x0C, 0x1C, 0x18, 0x38, 0x58, 0x78,
    0x68, 0x48, 0x28, 0x08, 0x14, 0x04, 0x02, 0x00
];
const BINK2_MV_BITS: &[u8] = &[
    1, 3, 5, 5, 7, 7, 7, 7,
    7, 7, 7, 7, 5, 5, 3, 4
];
const BINK2_MV_ESC: i8 = 42;
const BINK2_MV_SYMS: &[i8] = &[
    0, 1, 2, 3, 4, 5, 6, 7,
    -7, -6, -5, -4, -3, -2, -1, BINK2_MV_ESC
];

const BINK2_AC_VAL_CODES1: &[u16] = &[
    0x004, 0x01, 0x002, 0x00, 0x08, 0x18, 0xF8, 0x178,
    0x138, 0x38, 0x1B8, 0x78, 0xB8
];
const BINK2_AC_VAL_BITS1: &[u8] = &[ 3, 1, 2, 4, 5, 6, 8, 9, 9, 9, 9, 9, 9 ];
const BINK2_AC_VAL_CODES2: &[u16] = &[
    0x0A, 0x001, 0x004, 0x08, 0x06, 0x00, 0x02, 0x1A,
    0x2A, 0x16A, 0x1EA, 0x6A, 0xEA
];
const BINK2_AC_VAL_BITS2: &[u8] = &[ 6, 1, 3, 4, 3, 4, 4, 5, 7, 9, 9, 9, 9 ];
const BINK2_AC_SKIP_CODES1: &[u16] = &[
    0x00, 0x001, 0x0D, 0x15, 0x45, 0x85, 0xA5, 0x165,
    0x65, 0x1E5, 0xE5, 0x25, 0x03, 0x05
];
const BINK2_AC_SKIP_BITS1: &[u8] = &[ 1, 3, 4, 5, 7, 8, 8, 9, 9, 9, 9, 8, 2, 8 ];
const BINK2_AC_SKIP_CODES2: &[u16] = &[
    0x00, 0x01, 0x003, 0x07, 0x1F, 0x1B, 0x0F, 0x2F,
    0x5B, 0xDB, 0x1DB, 0x3B, 0x05, 0x0B
];
const BINK2_AC_SKIP_BITS2: &[u8] = &[ 1, 3, 4, 4, 5, 7, 6, 6, 8, 9, 9, 6, 3, 5 ];
const BINK2_QUANT_CODES: &[u8] = &[
    0x01, 0x02, 0x04, 0x08, 0x10, 0x30, 0x50, 0x70,
    0x00, 0x20, 0x40, 0x60, 0x80, 0xA0, 0xC0, 0xE0
];
const BINK2_QUANT_BITS: &[u8] = &[ 1, 2, 3, 4, 7, 7, 7, 7, 8, 8, 8, 8, 8, 8, 8, 8 ];

const BINK2_OLD_QUANTS: [f32; 16] = [
    1.0, 2.0, 2.5, 3.0, 3.5, 4.0, 6.0, 7.0, 8.0, 12.0, 16.0, 24.0, 32.0, 48.0, 64.0, 128.0
];
const BINK2_OLD_DC_QUANTS: [i32; 16] = [
    4, 4, 4, 4, 4, 6, 7, 8, 10, 12, 16, 24, 32, 48, 64, 128
];
const BINK2_DC_EXTRA_BITS: [u8; 16] = [ 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 3, 3, 4, 4, 5, 6 ];
const BINK2_QMAT_INTRA_LUMA_OLD: &[f32; 64] = &[
    0.125,    0.208056, 0.22864901, 0.205778, 0.22499999, 0.235708, 0.33148301,  0.248309,
    0.190718, 0.288582, 0.29449099, 0.34658501, 0.38143599, 0.47678301, 0.600528, 0.44008601,
    0.16332, 0.31714499, 0.34142101, 0.42249799, 0.604285, 0.70576, 0.68942899, 0.42807001,
    0.235175, 0.38735899, 0.46090701, 0.50122303, 0.82311302, 0.73910397, 0.69206202, 0.39741901,
    0.30000001, 0.45078799, 0.65328097, 0.74962097, 0.85000002, 0.79551601, 0.69678998, 0.38625899,
    0.392847, 0.79009801, 0.73142397, 1.004719, 1.070509, 0.80251199, 0.64313799, 0.27096599,
    0.34501299, 0.56299502, 0.60987997, 0.636379, 0.69678998, 0.60061598, 0.43934, 0.19224399,
    0.210373, 0.26309499, 0.252336, 0.25142801, 0.265553, 0.24928901, 0.188511, 0.094199002
];
const BINK2_QMAT_CHROMA_OLD: &[f32; 64] = &[
    0.125, 0.17338, 0.217761, 0.383793, 0.6875, 0.54016501, 0.37207201, 0.18968099,
    0.17338, 0.28056601, 0.32721299, 0.74753499, 0.95358998, 0.74923098, 0.51607901, 0.26309499,
    0.217761, 0.32721299, 0.66387498, 1.056244, 0.89826202, 0.70576, 0.48613599, 0.24783,
    0.383793, 0.74753499, 1.056244, 0.95059502, 0.80841398, 0.635167, 0.437511, 0.223041,
    0.6875, 0.95358998, 0.89826202, 0.80841398, 0.6875, 0.54016501, 0.37207201, 0.18968099,
    0.54016501, 0.74923098, 0.70576, 0.635167, 0.54016501, 0.42440501, 0.292335, 0.149031,
    0.37207201, 0.51607901, 0.48613599, 0.437511, 0.37207201, 0.292335, 0.201364, 0.102655,
    0.18968099, 0.26309499, 0.24783, 0.223041, 0.18968099, 0.149031, 0.102655, 0.052333001
];
const BINK2_QMAT_INTER_LUMA_OLD: &[f32; 64] = &[
    0.125, 0.17338, 0.16332, 0.146984, 0.128475, 0.106393, 0.077045999, 0.043109,
    0.17338, 0.240485, 0.226532, 0.20387299, 0.17820001, 0.147571, 0.109474, 0.062454,
    0.16332, 0.226532, 0.219321, 0.202722, 0.181465, 0.149711, 0.112943, 0.062583998,
    0.146984, 0.20387299, 0.202722, 0.201647, 0.183731, 0.15397599, 0.11711, 0.065334998,
    0.128475, 0.17820001, 0.181465, 0.183731, 0.17708801, 0.155499, 0.120267, 0.068016,
    0.106393, 0.147571, 0.149711, 0.15397599, 0.155499, 0.14575601, 0.116636, 0.068494998,
    0.077045999, 0.109474, 0.112943, 0.11711, 0.120267, 0.116636, 0.098646, 0.060141001,
    0.043109, 0.062454, 0.062583998, 0.065334998, 0.068016, 0.068494998, 0.060141001, 0.038853001
];
