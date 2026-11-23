use nihav_core::frame::{NAVideoBuffer, NAVideoBufferRef, NASimpleVideoFrame};
use super::{BlockDSP, CBPInfo, MV};
use super::super::blockdsp;
use super::data::H263_CHROMA_ROUND;

/*const W1: i32 = 22725;
const W2: i32 = 21407;
const W3: i32 = 19266;
const W4: i32 = 16383;
const W5: i32 = 12873;
const W6: i32 =  8867;
const W7: i32 =  4520;

const ROW_SHIFT: u8 = 11;
const COL_SHIFT: u8 = 20;

fn idct_row(row: &mut [i16]) {
    let in0 = row[0] as i32;
    let in1 = row[1] as i32;
    let in2 = row[2] as i32;
    let in3 = row[3] as i32;
    let in4 = row[4] as i32;
    let in5 = row[5] as i32;
    let in6 = row[6] as i32;
    let in7 = row[7] as i32;

    let mut a0 = in0 * W1 + (1 << (ROW_SHIFT - 1));
    let mut a1 = a0;
    let mut a2 = a0;
    let mut a3 = a0;

    a0 += W2 * in2;
    a1 += W6 * in2;
    a2 -= W6 * in2;
    a3 -= W2 * in2;

    let mut b0 = W1 * in1 + W3 * in3;
    let mut b1 = W3 * in1 - W7 * in3;
    let mut b2 = W5 * in1 - W1 * in3;
    let mut b3 = W7 * in1 - W5 * in3;

    a0 += W4 * in4 + W6 * in6;
    a1 -= W4 * in4 + W2 * in6;
    a2 -= W4 * in4 - W2 * in6;
    a3 += W4 * in4 - W6 * in6;

    b0 += W5 * in5 + W7 * in7;
    b1 -= W1 * in5 + W5 * in7;
    b2 += W7 * in5 + W3 * in7;
    b3 += W3 * in5 - W1 * in7;

    row[0] = ((a0 + b0) >> ROW_SHIFT) as i16;
    row[7] = ((a0 - b0) >> ROW_SHIFT) as i16;
    row[1] = ((a1 + b1) >> ROW_SHIFT) as i16;
    row[6] = ((a1 - b1) >> ROW_SHIFT) as i16;
    row[2] = ((a2 + b2) >> ROW_SHIFT) as i16;
    row[5] = ((a2 - b2) >> ROW_SHIFT) as i16;
    row[3] = ((a3 + b3) >> ROW_SHIFT) as i16;
    row[4] = ((a3 - b3) >> ROW_SHIFT) as i16;
}

fn idct_col(blk: &mut [i16; 64], off: usize) {
    let in0 = blk[off + 0*8] as i32;
    let in1 = blk[off + 1*8] as i32;
    let in2 = blk[off + 2*8] as i32;
    let in3 = blk[off + 3*8] as i32;
    let in4 = blk[off + 4*8] as i32;
    let in5 = blk[off + 5*8] as i32;
    let in6 = blk[off + 6*8] as i32;
    let in7 = blk[off + 7*8] as i32;

    let mut a0 = in0 * W1 + (1 << (COL_SHIFT - 1));
    let mut a1 = a0;
    let mut a2 = a0;
    let mut a3 = a0;

    a0 += W2 * in2;
    a1 += W6 * in2;
    a2 -= W6 * in2;
    a3 -= W2 * in2;

    let mut b0 = W1 * in1 + W3 * in3;
    let mut b1 = W3 * in1 - W7 * in3;
    let mut b2 = W5 * in1 - W1 * in3;
    let mut b3 = W7 * in1 - W5 * in3;

    a0 += W4 * in4 + W6 * in6;
    a1 -= W4 * in4 + W2 * in6;
    a2 -= W4 * in4 - W2 * in6;
    a3 += W4 * in4 - W6 * in6;

    b0 += W5 * in5 + W7 * in7;
    b1 -= W1 * in5 + W5 * in7;
    b2 += W7 * in5 + W3 * in7;
    b3 += W3 * in5 - W1 * in7;

    blk[off + 0*8] = ((a0 + b0) >> COL_SHIFT) as i16;
    blk[off + 7*8] = ((a0 - b0) >> COL_SHIFT) as i16;
    blk[off + 1*8] = ((a1 + b1) >> COL_SHIFT) as i16;
    blk[off + 6*8] = ((a1 - b1) >> COL_SHIFT) as i16;
    blk[off + 2*8] = ((a2 + b2) >> COL_SHIFT) as i16;
    blk[off + 5*8] = ((a2 - b2) >> COL_SHIFT) as i16;
    blk[off + 3*8] = ((a3 + b3) >> COL_SHIFT) as i16;
    blk[off + 4*8] = ((a3 - b3) >> COL_SHIFT) as i16;
}

#[allow(dead_code)]
pub fn h263_idct(blk: &mut [i16; 64]) {
    for i in 0..8 { idct_row(&mut blk[i*8..(i+1)*8]); }
    for i in 0..8 { idct_col(blk, i); }
}*/

const W1: i32 = 2841;
const W2: i32 = 2676;
const W3: i32 = 2408;
const W5: i32 = 1609;
const W6: i32 = 1108;
const W7: i32 =  565;
const W8: i32 =  181;

const ROW_SHIFT: u8 = 8;
const COL_SHIFT: u8 = 14;

#[allow(clippy::erasing_op)]
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

#[allow(dead_code)]
pub fn h263_idct(blk: &mut [i16; 64]) {
    for i in 0..8 { idct_row(&mut blk[i*8..(i+1)*8]); }
    for i in 0..8 { idct_col(blk, i); }
}

struct IDCTAnnexW {}

impl IDCTAnnexW {
    const CPO8: i32     = 0x539f;
    const SPO8: i32     = 0x4546;
    const CPO16: i32    = 0x7d8a;
    const SPO16: i32    = 0x18f9;
    const C3PO16: i32   = 0x6a6e;
    const S3PO16: i32   = 0x471d;
    const OOR2: i32     = 0x5a82;

    fn rotate(a: i32, b: i32, c: i32, s: i32, cs: i8, ss: i8) -> (i32, i32) {
        let (t00, t10) = if cs > 0 {
                ((a * c) >>  cs, (b * c) >>  cs)
            } else {
                ((a * c) << -cs, (b * c) << -cs)
            };
        let (t01, t11) = if ss > 0 {
                ((a * s) >>  ss, (b * s) >>  ss)
            } else {
                ((a * s) << -ss, (b * s) << -ss)
            };
        ((t01 - t10 + 0x7FFF) >> 16, (t00 + t11 + 0x7FFF) >> 16)
    }

    fn bfly(a: i32, b: i32) -> (i32, i32) { (a + b, a - b) }

    fn idct_row(dst: &mut [i32; 64], src: &[i16; 64]) {
        for (drow, srow) in dst.chunks_mut(8).zip(src.chunks(8)) {
            let s0 = i32::from(srow[0]) << 4;
            let s1 = i32::from(srow[1]) << 4;
            let s2 = i32::from(srow[2]) << 4;
            let s3 = i32::from(srow[3]) << 4;
            let s4 = i32::from(srow[4]) << 4;
            let s5 = i32::from(srow[5]) << 4;
            let s6 = i32::from(srow[6]) << 4;
            let s7 = i32::from(srow[7]) << 4;

            let (s2, s6) = Self::rotate(s2, s6, Self::CPO8,   Self::SPO8,   -2, -1);
            let (s1, s7) = Self::rotate(s1, s7, Self::CPO16,  Self::SPO16,  -1, -1);
            let (s3, s5) = Self::rotate(s3, s5, Self::C3PO16, Self::S3PO16, -1, -1);
            let (s0, s4) = Self::bfly(s0, s4);

            let (s3, s1) = Self::bfly(s1, s3);
            let (s5, s7) = Self::bfly(s7, s5);
            let (s0, s6) = Self::bfly(s0, s6);
            let (s4, s2) = Self::bfly(s4, s2);

            let (s3, s7) = Self::bfly(s7, s3);
            let s1 = (s1 * Self::OOR2 * 4).saturating_add(0x7FFF) >> 16;
            let s5 = (s5 * Self::OOR2 * 4).saturating_add(0x7FFF) >> 16;

            drow[1] = s4 + s3;
            drow[6] = s4 - s3;
            drow[2] = s2 + s7;
            drow[5] = s2 - s7;
            drow[0] = s0 + s5;
            drow[7] = s0 - s5;
            drow[3] = s6 + s1;
            drow[4] = s6 - s1;
        }
    }

    #[allow(clippy::erasing_op)]
    #[allow(clippy::identity_op)]
    fn idct_col(dst: &mut [i16; 64], src: &[i32; 64]) {
        for i in 0..8 {
            let s0 = src[i + 8 * 0];
            let s1 = src[i + 8 * 1];
            let s2 = src[i + 8 * 2];
            let s3 = src[i + 8 * 3];
            let s4 = src[i + 8 * 4];
            let s5 = src[i + 8 * 5];
            let s6 = src[i + 8 * 6];
            let s7 = src[i + 8 * 7];

            let (s2, s6) = Self::rotate(s2, s6, Self::CPO8,   Self::SPO8,   -1, 0);
            let (s1, s7) = Self::rotate(s1, s7, Self::CPO16,  Self::SPO16,   0, 0);
            let (s3, s5) = Self::rotate(s3, s5, Self::C3PO16, Self::S3PO16,  0, 0);
            let (a, b) = Self::bfly(s0, s4);
            let (s0, s4) = if s4 >= 0 {
                    (a >> 1, b >> 1)
                } else {
                    ((a + 1) >> 1, (b + 1) >> 1)
                };

            let (s3, s1) = Self::bfly(s1, s3);
            let (s5, s7) = Self::bfly(s7, s5);
            let (s0, s6) = Self::bfly(s0, s6);
            let (s4, s2) = Self::bfly(s4, s2);

            let (s3, s7) = Self::bfly(s7, s3);
            let s1 = (s1 * Self::OOR2 * 4).saturating_add(0x7FFF) >> 16;
            let s5 = (s5 * Self::OOR2 * 4).saturating_add(0x7FFF) >> 16;

            dst[i + 8 * 1] = ((s4 + s3) >> 6) as i16;
            dst[i + 8 * 6] = ((s4 - s3) >> 6) as i16;
            dst[i + 8 * 2] = ((s2 + s7) >> 6) as i16;
            dst[i + 8 * 5] = ((s2 - s7) >> 6) as i16;
            dst[i + 8 * 0] = ((s0 + s5) >> 6) as i16;
            dst[i + 8 * 7] = ((s0 - s5) >> 6) as i16;
            dst[i + 8 * 3] = ((s6 + s1) >> 6) as i16;
            dst[i + 8 * 4] = ((s6 - s1) >> 6) as i16;
        }
    }
}
#[allow(dead_code)]
pub fn h263_annex_w_idct(blk: &mut [i16; 64]) {
    let mut tmp = [0i32; 64];
    IDCTAnnexW::idct_row(&mut tmp, blk);
    IDCTAnnexW::idct_col(blk, &tmp);
}

fn h263_interp00(dst: &mut [u8], dstride: usize, src: &[u8], sstride: usize, bw: usize, bh: usize)
{
    let mut didx = 0;
    let mut sidx = 0;
    for _ in 0..bh {
        for x in 0..bw { dst[didx + x] = src[sidx + x]; }
        didx += dstride;
        sidx += sstride;
    }
}

fn h263_interp01(dst: &mut [u8], dstride: usize, src: &[u8], sstride: usize, bw: usize, bh: usize)
{
    let mut didx = 0;
    let mut sidx = 0;
    for _ in 0..bh {
        for x in 0..bw { dst[didx + x] = (((src[sidx + x] as u16) + (src[sidx + x + 1] as u16) + 1) >> 1) as u8; }
        didx += dstride;
        sidx += sstride;
    }
}

fn h263_interp10(dst: &mut [u8], dstride: usize, src: &[u8], sstride: usize, bw: usize, bh: usize)
{
    let mut didx = 0;
    let mut sidx = 0;
    for _ in 0..bh {
        for x in 0..bw { dst[didx + x] = (((src[sidx + x] as u16) + (src[sidx + x + sstride] as u16) + 1) >> 1) as u8; }
        didx += dstride;
        sidx += sstride;
    }
}

fn h263_interp11(dst: &mut [u8], dstride: usize, src: &[u8], sstride: usize, bw: usize, bh: usize)
{
    let mut didx = 0;
    let mut sidx = 0;
    for _ in 0..bh {
        for x in 0..bw {
            dst[didx + x] = (((src[sidx + x] as u16) +
                              (src[sidx + x + 1] as u16) +
                              (src[sidx + x + sstride] as u16) +
                              (src[sidx + x + sstride + 1] as u16) + 2) >> 2) as u8;
        }
        didx += dstride;
        sidx += sstride;
    }
}

pub const H263_INTERP_FUNCS: &[blockdsp::BlkInterpFunc] = &[
        h263_interp00, h263_interp01, h263_interp10, h263_interp11 ];

fn h263_interp00_avg(dst: &mut [u8], dstride: usize, src: &[u8], sstride: usize, bw: usize, bh: usize)
{
    let mut didx = 0;
    let mut sidx = 0;
    for _ in 0..bh {
        for x in 0..bw {
            let a = dst[didx + x] as u16;
            let b = src[sidx + x] as u16;
            dst[didx + x] = ((a + b + 1) >> 1) as u8;
        }
        didx += dstride;
        sidx += sstride;
    }
}

fn h263_interp01_avg(dst: &mut [u8], dstride: usize, src: &[u8], sstride: usize, bw: usize, bh: usize)
{
    let mut didx = 0;
    let mut sidx = 0;
    for _ in 0..bh {
        for x in 0..bw {
            let a = dst[didx + x] as u16;
            let b = ((src[sidx + x] as u16) + (src[sidx + x + 1] as u16) + 1) >> 1;
            dst[didx + x] = ((a + b + 1) >> 1) as u8;
        }
        didx += dstride;
        sidx += sstride;
    }
}

fn h263_interp10_avg(dst: &mut [u8], dstride: usize, src: &[u8], sstride: usize, bw: usize, bh: usize)
{
    let mut didx = 0;
    let mut sidx = 0;
    for _ in 0..bh {
        for x in 0..bw {
            let a = dst[didx + x] as u16;
            let b = ((src[sidx + x] as u16) + (src[sidx + x + sstride] as u16) + 1) >> 1;
            dst[didx + x] = ((a + b + 1) >> 1) as u8;
        }
        didx += dstride;
        sidx += sstride;
    }
}

fn h263_interp11_avg(dst: &mut [u8], dstride: usize, src: &[u8], sstride: usize, bw: usize, bh: usize)
{
    let mut didx = 0;
    let mut sidx = 0;
    for _ in 0..bh {
        for x in 0..bw {
            let a = dst[didx + x] as u16;
            let b = ((src[sidx + x] as u16) +
                     (src[sidx + x + 1] as u16) +
                     (src[sidx + x + sstride] as u16) +
                     (src[sidx + x + sstride + 1] as u16) + 2) >> 2;
            dst[didx + x] = ((a + b + 1) >> 1) as u8;
        }
        didx += dstride;
        sidx += sstride;
    }
}

pub const H263_INTERP_AVG_FUNCS: &[blockdsp::BlkInterpFunc] = &[
        h263_interp00_avg, h263_interp01_avg, h263_interp10_avg, h263_interp11_avg ];

#[derive(Default)]
pub struct H263BlockDSP { }

impl H263BlockDSP {
    pub fn new() -> Self {
        H263BlockDSP { }
    }
}

#[allow(clippy::erasing_op)]
fn deblock_hor(buf: &mut NAVideoBuffer<u8>, comp: usize, strength: u8, off: usize) {
    let stride = buf.get_stride(comp);
    let dptr = buf.get_data_mut().unwrap();
    let buf = dptr.as_mut_slice();
    for x in 0..8 {
        let a = buf[off - 2 * stride + x] as i16;
        let b = buf[off - 1 * stride + x] as i16;
        let c = buf[off + 0 * stride + x] as i16;
        let d = buf[off + 1 * stride + x] as i16;
        let diff = ((a - d) + (c - b) * 4) / 8;
        if (diff != 0) && (diff > -24) && (diff < 24) {
            let d1a = (diff.abs() - 2 * (diff.abs() - (strength as i16)).max(0)).max(0);
            let d1  = if diff < 0 { -d1a } else { d1a };
            let hd1 = d1a / 2;
            let d2  = ((a - d) / 4).max(-hd1).min(hd1);

            buf[off - 2 * stride + x] = (a - d2) as u8;
            buf[off - 1 * stride + x] = (b + d1).max(0).min(255) as u8;
            buf[off + 0 * stride + x] = (c - d1).max(0).min(255) as u8;
            buf[off + 1 * stride + x] = (d + d2) as u8;
        }
    }
}

fn deblock_ver(buf: &mut NAVideoBuffer<u8>, comp: usize, strength: u8, off: usize) {
    let stride = buf.get_stride(comp);
    let dptr = buf.get_data_mut().unwrap();
    let buf = dptr.as_mut_slice();
    for y in 0..8 {
        let a = buf[off - 2 + y * stride] as i16;
        let b = buf[off - 1 + y * stride] as i16;
        let c = buf[off + 0 + y * stride] as i16;
        let d = buf[off + 1 + y * stride] as i16;
        let diff = (a - d + (c - b) * 4) / 8;
        if (diff != 0) && (diff > -24) && (diff < 24) {
            let d1a = (diff.abs() - 2 * (diff.abs() - (strength as i16)).max(0)).max(0);
            let d1  = if diff < 0 { -d1a } else { d1a };
            let hd1 = d1a / 2;
            let d2  = ((a - d) / 4).max(-hd1).min(hd1);

            buf[off - 2 + y * stride] = (a - d2) as u8;
            buf[off - 1 + y * stride] = (b + d1).max(0).min(255) as u8;
            buf[off     + y * stride] = (c - d1).max(0).min(255) as u8;
            buf[off + 1 + y * stride] = (d + d2) as u8;
        }
    }
}

const FILTER_STRENGTH: [u8; 32] = [
    1,  1,  2,  2,  3,  3,  4,  4,  4,  5,  5,  5,  6,  6,  7,  7,
    7,  8,  8,  8,  9,  9,  9, 10, 10, 10, 11, 11, 11, 12, 12, 12
];

pub fn h263_filter_row(buf: &mut NAVideoBuffer<u8>, mb_y: usize, mb_w: usize, cbpi: &CBPInfo) {
    let stride  = buf.get_stride(0);
    let mut off = buf.get_offset(0) + mb_y * 16 * stride;
    for mb_x in 0..mb_w {
        let coff = off;
        let coded0 = cbpi.is_coded(mb_x, 0);
        let coded1 = cbpi.is_coded(mb_x, 1);
        let q = cbpi.get_q(mb_w + mb_x);
        let strength = if q < 32 { FILTER_STRENGTH[q as usize] } else { 0 };
        if mb_y != 0 {
            if coded0 && cbpi.is_coded_top(mb_x, 0) { deblock_hor(buf, 0, strength, coff); }
            if coded1 && cbpi.is_coded_top(mb_x, 1) { deblock_hor(buf, 0, strength, coff + 8); }
        }
        let coff = off + 8 * stride;
        if cbpi.is_coded(mb_x, 2) && coded0 { deblock_hor(buf, 0, q, coff); }
        if cbpi.is_coded(mb_x, 3) && coded1 { deblock_hor(buf, 0, q, coff + 8); }
        off += 16;
    }
    let mut leftt = false;
    let mut leftc = false;
    let mut off = buf.get_offset(0) + mb_y * 16 * stride;
    for mb_x in 0..mb_w {
        let ctop0 = cbpi.is_coded_top(mb_x, 0);
        let ctop1 = cbpi.is_coded_top(mb_x, 0);
        let ccur0 = cbpi.is_coded(mb_x, 0);
        let ccur1 = cbpi.is_coded(mb_x, 1);
        let q = cbpi.get_q(mb_w + mb_x);
        let strength = if q < 32 { FILTER_STRENGTH[q as usize] } else { 0 };
        if mb_y != 0 {
            let coff = off - 8 * stride;
            let qtop = cbpi.get_q(mb_x);
            let strtop = if qtop < 32 { FILTER_STRENGTH[qtop as usize] } else { 0 };
            if leftt && ctop0 { deblock_ver(buf, 0, strtop, coff); }
            if ctop0 && ctop1 { deblock_ver(buf, 0, strtop, coff + 8); }
        }
        if leftc && ccur0 { deblock_ver(buf, 0, strength, off); }
        if ccur0 && ccur1 { deblock_ver(buf, 0, strength, off + 8); }
        leftt = ctop1;
        leftc = ccur1;
        off += 16;
    }
    let strideu = buf.get_stride(1);
    let stridev = buf.get_stride(2);
    let offu = buf.get_offset(1) + mb_y * 8 * strideu;
    let offv = buf.get_offset(2) + mb_y * 8 * stridev;
    if mb_y != 0 {
        for mb_x in 0..mb_w {
            let ctu = cbpi.is_coded_top(mb_x, 4);
            let ccu = cbpi.is_coded(mb_x, 4);
            let ctv = cbpi.is_coded_top(mb_x, 5);
            let ccv = cbpi.is_coded(mb_x, 5);
            let q = cbpi.get_q(mb_w + mb_x);
            let strength = if q < 32 { FILTER_STRENGTH[q as usize] } else { 0 };
            if ctu && ccu { deblock_hor(buf, 1, strength, offu + mb_x * 8); }
            if ctv && ccv { deblock_hor(buf, 2, strength, offv + mb_x * 8); }
        }
        let mut leftu = false;
        let mut leftv = false;
        let offu = buf.get_offset(1) + (mb_y - 1) * 8 * strideu;
        let offv = buf.get_offset(2) + (mb_y - 1) * 8 * stridev;
        for mb_x in 0..mb_w {
            let ctu = cbpi.is_coded_top(mb_x, 4);
            let ctv = cbpi.is_coded_top(mb_x, 5);
            let qt = cbpi.get_q(mb_x);
            let strt = if qt < 32 { FILTER_STRENGTH[qt as usize] } else { 0 };
            if leftu && ctu { deblock_ver(buf, 1, strt, offu + mb_x * 8); }
            if leftv && ctv { deblock_ver(buf, 2, strt, offv + mb_x * 8); }
            leftu = ctu;
            leftv = ctv;
        }
    }
}

impl BlockDSP for H263BlockDSP {
    fn idct(&self, blk: &mut [i16; 64]) {
        h263_idct(blk)
    }
    fn copy_blocks(&self, dst: &mut NAVideoBuffer<u8>, src: NAVideoBufferRef<u8>, xpos: usize, ypos: usize, mv: MV) {
        let mode = ((mv.x & 1) + (mv.y & 1) * 2) as usize;
        let cmode = (if (mv.x & 3) != 0 { 1 } else { 0 }) + (if (mv.y & 3) != 0 { 2 } else { 0 });

        let mut dst = NASimpleVideoFrame::from_video_buf(dst).unwrap();

        blockdsp::copy_block(&mut dst, src.clone(), 0, xpos, ypos, mv.x >> 1, mv.y >> 1, 16, 16, 0, 1, mode, H263_INTERP_FUNCS);
        blockdsp::copy_block(&mut dst, src.clone(), 1, xpos >> 1, ypos >> 1, mv.x >> 2, mv.y >> 2, 8, 8, 0, 1, cmode, H263_INTERP_FUNCS);
        blockdsp::copy_block(&mut dst, src,         2, xpos >> 1, ypos >> 1, mv.x >> 2, mv.y >> 2, 8, 8, 0, 1, cmode, H263_INTERP_FUNCS);
    }
    fn copy_blocks8x8(&self, dst: &mut NAVideoBuffer<u8>, src: NAVideoBufferRef<u8>, xpos: usize, ypos: usize, mvs: &[MV; 4]) {
        let mut dst = NASimpleVideoFrame::from_video_buf(dst).unwrap();

        for i in 0..4 {
            let xadd = (i & 1) * 8;
            let yadd = (i & 2) * 4;
            let mode = ((mvs[i].x & 1) + (mvs[i].y & 1) * 2) as usize;

            blockdsp::copy_block(&mut dst, src.clone(), 0, xpos + xadd, ypos + yadd, mvs[i].x >> 1, mvs[i].y >> 1, 8, 8, 0, 1, mode, H263_INTERP_FUNCS);
        }

        let sum_mv = mvs[0] + mvs[1] + mvs[2] + mvs[3];
        let cmx = (sum_mv.x >> 3) + H263_CHROMA_ROUND[(sum_mv.x & 0xF) as usize];
        let cmy = (sum_mv.y >> 3) + H263_CHROMA_ROUND[(sum_mv.y & 0xF) as usize];
        let mode = ((cmx & 1) + (cmy & 1) * 2) as usize;
        for plane in 1..3 {
            blockdsp::copy_block(&mut dst, src.clone(), plane, xpos >> 1, ypos >> 1, cmx >> 1, cmy >> 1, 8, 8, 0, 1, mode, H263_INTERP_FUNCS);
        }
    }
    fn avg_blocks(&self, dst: &mut NAVideoBuffer<u8>, src: NAVideoBufferRef<u8>, xpos: usize, ypos: usize, mv: MV) {
        let mode = ((mv.x & 1) + (mv.y & 1) * 2) as usize;
        let cmode = (if (mv.x & 3) != 0 { 1 } else { 0 }) + (if (mv.y & 3) != 0 { 2 } else { 0 });

        let mut dst = NASimpleVideoFrame::from_video_buf(dst).unwrap();

        blockdsp::copy_block(&mut dst, src.clone(), 0, xpos, ypos, mv.x >> 1, mv.y >> 1, 16, 16, 0, 1, mode, H263_INTERP_AVG_FUNCS);
        blockdsp::copy_block(&mut dst, src.clone(), 1, xpos >> 1, ypos >> 1, mv.x >> 2, mv.y >> 2, 8, 8, 0, 1, cmode, H263_INTERP_AVG_FUNCS);
        blockdsp::copy_block(&mut dst, src,         2, xpos >> 1, ypos >> 1, mv.x >> 2, mv.y >> 2, 8, 8, 0, 1, cmode, H263_INTERP_AVG_FUNCS);
    }
    fn avg_blocks8x8(&self, dst: &mut NAVideoBuffer<u8>, src: NAVideoBufferRef<u8>, xpos: usize, ypos: usize, mvs: &[MV; 4]) {
        let mut dst = NASimpleVideoFrame::from_video_buf(dst).unwrap();

        for i in 0..4 {
            let xadd = (i & 1) * 8;
            let yadd = (i & 2) * 4;
            let mode = ((mvs[i].x & 1) + (mvs[i].y & 1) * 2) as usize;

            blockdsp::copy_block(&mut dst, src.clone(), 0, xpos + xadd, ypos + yadd, mvs[i].x >> 1, mvs[i].y >> 1, 8, 8, 0, 1, mode, H263_INTERP_AVG_FUNCS);
        }

        let sum_mv = mvs[0] + mvs[1] + mvs[2] + mvs[3];
        let cmx = (sum_mv.x >> 3) + H263_CHROMA_ROUND[(sum_mv.x & 0xF) as usize];
        let cmy = (sum_mv.y >> 3) + H263_CHROMA_ROUND[(sum_mv.y & 0xF) as usize];
        let mode = ((cmx & 1) + (cmy & 1) * 2) as usize;
        for plane in 1..3 {
            blockdsp::copy_block(&mut dst, src.clone(), plane, xpos >> 1, ypos >> 1, cmx >> 1, cmy >> 1, 8, 8, 0, 1, mode, H263_INTERP_AVG_FUNCS);
        }
    }
    fn filter_row(&self, buf: &mut NAVideoBuffer<u8>, mb_y: usize, mb_w: usize, cbpi: &CBPInfo) {
        h263_filter_row(buf, mb_y, mb_w, cbpi)
    }
}

macro_rules! obmc_filter {
    ($src: expr, $base_off: expr, $off0: expr, $w0: expr, $off1: expr, $w1: expr, $off2: expr, $w2: expr) => ({
        let a = $src[$base_off + $off0] as u16;
        let b = $src[$base_off + $off1] as u16;
        let c = $src[$base_off + $off2] as u16;
        ((a * $w0 + b * $w1 + c * $w2 + 4) >> 3) as u8
    })
}
pub fn obmc_filter(dst: &mut [u8], dstride: usize, src: &[u8], sstride: usize) {
    let top_off    = 8  + sstride * 0;
    let left_off   = 0  + sstride * 8;
    let right_off  = 16 + sstride * 8;
    let bottom_off = 8  + sstride * 16;
    let cur_off    = 8  + sstride * 8;

    let mut doff = 0;
    let mut soff = 0;

    dst[doff + 0] = obmc_filter!(src, soff + 0, left_off,  2, top_off, 2, cur_off, 4);
    dst[doff + 1] = obmc_filter!(src, soff + 1, left_off,  1, top_off, 2, cur_off, 5);
    dst[doff + 2] = obmc_filter!(src, soff + 2, left_off,  1, top_off, 2, cur_off, 5);
    dst[doff + 3] = obmc_filter!(src, soff + 3, left_off,  1, top_off, 2, cur_off, 5);
    dst[doff + 4] = obmc_filter!(src, soff + 4, right_off, 1, top_off, 2, cur_off, 5);
    dst[doff + 5] = obmc_filter!(src, soff + 5, right_off, 1, top_off, 2, cur_off, 5);
    dst[doff + 6] = obmc_filter!(src, soff + 6, right_off, 1, top_off, 2, cur_off, 5);
    dst[doff + 7] = obmc_filter!(src, soff + 7, right_off, 2, top_off, 2, cur_off, 4);
    doff += dstride;
    soff += sstride;

    dst[doff + 0] = obmc_filter!(src, soff + 0, left_off,  2, top_off, 1, cur_off, 5);
    dst[doff + 1] = obmc_filter!(src, soff + 1, left_off,  2, top_off, 1, cur_off, 5);
    dst[doff + 2] = obmc_filter!(src, soff + 2, left_off,  1, top_off, 2, cur_off, 5);
    dst[doff + 3] = obmc_filter!(src, soff + 3, left_off,  1, top_off, 2, cur_off, 5);
    dst[doff + 4] = obmc_filter!(src, soff + 4, right_off, 1, top_off, 2, cur_off, 5);
    dst[doff + 5] = obmc_filter!(src, soff + 5, right_off, 1, top_off, 2, cur_off, 5);
    dst[doff + 6] = obmc_filter!(src, soff + 6, right_off, 2, top_off, 1, cur_off, 5);
    dst[doff + 7] = obmc_filter!(src, soff + 7, right_off, 2, top_off, 1, cur_off, 5);
    doff += dstride;
    soff += sstride;

    dst[doff + 0] = obmc_filter!(src, soff + 0, left_off,  2, top_off, 1, cur_off, 5);
    dst[doff + 1] = obmc_filter!(src, soff + 1, left_off,  2, top_off, 1, cur_off, 5);
    dst[doff + 2] = obmc_filter!(src, soff + 2, left_off,  1, top_off, 1, cur_off, 6);
    dst[doff + 3] = obmc_filter!(src, soff + 3, left_off,  1, top_off, 1, cur_off, 6);
    dst[doff + 4] = obmc_filter!(src, soff + 4, right_off, 1, top_off, 1, cur_off, 6);
    dst[doff + 5] = obmc_filter!(src, soff + 5, right_off, 1, top_off, 1, cur_off, 6);
    dst[doff + 6] = obmc_filter!(src, soff + 6, right_off, 2, top_off, 1, cur_off, 5);
    dst[doff + 7] = obmc_filter!(src, soff + 7, right_off, 2, top_off, 1, cur_off, 5);
    doff += dstride;
    soff += sstride;

    dst[doff + 0] = obmc_filter!(src, soff + 0, left_off,  2, top_off, 1, cur_off, 5);
    dst[doff + 1] = obmc_filter!(src, soff + 1, left_off,  2, top_off, 1, cur_off, 5);
    dst[doff + 2] = obmc_filter!(src, soff + 2, left_off,  1, top_off, 1, cur_off, 6);
    dst[doff + 3] = obmc_filter!(src, soff + 3, left_off,  1, top_off, 1, cur_off, 6);
    dst[doff + 4] = obmc_filter!(src, soff + 4, right_off, 1, top_off, 1, cur_off, 6);
    dst[doff + 5] = obmc_filter!(src, soff + 5, right_off, 1, top_off, 1, cur_off, 6);
    dst[doff + 6] = obmc_filter!(src, soff + 6, right_off, 2, top_off, 1, cur_off, 5);
    dst[doff + 7] = obmc_filter!(src, soff + 7, right_off, 2, top_off, 1, cur_off, 5);
    doff += dstride;
    soff += sstride;

    dst[doff + 0] = obmc_filter!(src, soff + 0, left_off,  2, bottom_off, 1, cur_off, 5);
    dst[doff + 1] = obmc_filter!(src, soff + 1, left_off,  2, bottom_off, 1, cur_off, 5);
    dst[doff + 2] = obmc_filter!(src, soff + 2, left_off,  1, bottom_off, 1, cur_off, 6);
    dst[doff + 3] = obmc_filter!(src, soff + 3, left_off,  1, bottom_off, 1, cur_off, 6);
    dst[doff + 4] = obmc_filter!(src, soff + 4, right_off, 1, bottom_off, 1, cur_off, 6);
    dst[doff + 5] = obmc_filter!(src, soff + 5, right_off, 1, bottom_off, 1, cur_off, 6);
    dst[doff + 6] = obmc_filter!(src, soff + 6, right_off, 2, bottom_off, 1, cur_off, 5);
    dst[doff + 7] = obmc_filter!(src, soff + 7, right_off, 2, bottom_off, 1, cur_off, 5);
    doff += dstride;
    soff += sstride;

    dst[doff + 0] = obmc_filter!(src, soff + 0, left_off,  2, bottom_off, 1, cur_off, 5);
    dst[doff + 1] = obmc_filter!(src, soff + 1, left_off,  2, bottom_off, 1, cur_off, 5);
    dst[doff + 2] = obmc_filter!(src, soff + 2, left_off,  1, bottom_off, 1, cur_off, 6);
    dst[doff + 3] = obmc_filter!(src, soff + 3, left_off,  1, bottom_off, 1, cur_off, 6);
    dst[doff + 4] = obmc_filter!(src, soff + 4, right_off, 1, bottom_off, 1, cur_off, 6);
    dst[doff + 5] = obmc_filter!(src, soff + 5, right_off, 1, bottom_off, 1, cur_off, 6);
    dst[doff + 6] = obmc_filter!(src, soff + 6, right_off, 2, bottom_off, 1, cur_off, 5);
    dst[doff + 7] = obmc_filter!(src, soff + 7, right_off, 2, bottom_off, 1, cur_off, 5);
    doff += dstride;
    soff += sstride;

    dst[doff + 0] = obmc_filter!(src, soff + 0, left_off,  2, bottom_off, 1, cur_off, 5);
    dst[doff + 1] = obmc_filter!(src, soff + 1, left_off,  2, bottom_off, 1, cur_off, 5);
    dst[doff + 2] = obmc_filter!(src, soff + 2, left_off,  1, bottom_off, 2, cur_off, 5);
    dst[doff + 3] = obmc_filter!(src, soff + 3, left_off,  1, bottom_off, 2, cur_off, 5);
    dst[doff + 4] = obmc_filter!(src, soff + 4, right_off, 1, bottom_off, 2, cur_off, 5);
    dst[doff + 5] = obmc_filter!(src, soff + 5, right_off, 1, bottom_off, 2, cur_off, 5);
    dst[doff + 6] = obmc_filter!(src, soff + 6, right_off, 2, bottom_off, 1, cur_off, 5);
    dst[doff + 7] = obmc_filter!(src, soff + 7, right_off, 2, bottom_off, 1, cur_off, 5);
    doff += dstride;
    soff += sstride;

    dst[doff + 0] = obmc_filter!(src, soff + 0, left_off,  2, bottom_off, 2, cur_off, 4);
    dst[doff + 1] = obmc_filter!(src, soff + 1, left_off,  1, bottom_off, 2, cur_off, 5);
    dst[doff + 2] = obmc_filter!(src, soff + 2, left_off,  1, bottom_off, 2, cur_off, 5);
    dst[doff + 3] = obmc_filter!(src, soff + 3, left_off,  1, bottom_off, 2, cur_off, 5);
    dst[doff + 4] = obmc_filter!(src, soff + 4, right_off, 1, bottom_off, 2, cur_off, 5);
    dst[doff + 5] = obmc_filter!(src, soff + 5, right_off, 1, bottom_off, 2, cur_off, 5);
    dst[doff + 6] = obmc_filter!(src, soff + 6, right_off, 1, bottom_off, 2, cur_off, 5);
    dst[doff + 7] = obmc_filter!(src, soff + 7, right_off, 2, bottom_off, 2, cur_off, 4);
}
