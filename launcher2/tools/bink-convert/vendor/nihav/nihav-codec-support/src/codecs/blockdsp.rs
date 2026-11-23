//! Various pixel block manipulation functions.
use nihav_core::frame::*;

/// Puts YUV420 16x16 macroblock data onto picture in the requested place.
pub fn put_blocks(buf: &mut NAVideoBuffer<u8>, xpos: usize, ypos: usize, blk: &[[i16;64]; 6]) {
    let stridey = buf.get_stride(0);
    let strideu = buf.get_stride(1);
    let stridev = buf.get_stride(2);
    let mut idxy = buf.get_offset(0) + xpos * 16 + ypos * 16 * stridey;
    let mut idxu = buf.get_offset(1) + xpos *  8 + ypos *  8 * strideu;
    let mut idxv = buf.get_offset(2) + xpos *  8 + ypos *  8 * stridev;

    let data = buf.get_data_mut().unwrap();
    let framebuf: &mut [u8] = data.as_mut_slice();

    for j in 0..8 {
        for k in 0..8 {
            let mut v = blk[0][k + j * 8];
            if v < 0 { v = 0; } else if v > 255 { v = 255; }
            framebuf[idxy + k] = v as u8;
        }
        for k in 0..8 {
            let mut v = blk[1][k + j * 8];
            if v < 0 { v = 0; } else if v > 255 { v = 255; }
            framebuf[idxy + k + 8] = v as u8;
        }
        idxy += stridey;
    }
    for j in 0..8 {
        for k in 0..8 {
            let mut v = blk[2][k + j * 8];
            if v < 0 { v = 0; } else if v > 255 { v = 255; }
            framebuf[idxy + k] = v as u8;
        }
        for k in 0..8 {
            let mut v = blk[3][k + j * 8];
            if v < 0 { v = 0; } else if v > 255 { v = 255; }
            framebuf[idxy + k + 8] = v as u8;
        }
        idxy += stridey;
    }

    for j in 0..8 {
        for k in 0..8 {
            let mut v = blk[4][k + j * 8];
            if v < 0 { v = 0; } else if v > 255 { v = 255; }
            framebuf[idxu + k] = v as u8;
        }
        for k in 0..8 {
            let mut v = blk[5][k + j * 8];
            if v < 0 { v = 0; } else if v > 255 { v = 255; }
            framebuf[idxv + k] = v as u8;
        }
        idxu += strideu;
        idxv += stridev;
    }
}

/// Adds YUV420 16x16 macroblock coefficients to the picture in the requested place.
pub fn add_blocks(buf: &mut NAVideoBuffer<u8>, xpos: usize, ypos: usize, blk: &[[i16;64]; 6]) {
    let stridey = buf.get_stride(0);
    let strideu = buf.get_stride(1);
    let stridev = buf.get_stride(2);
    let mut idxy = buf.get_offset(0) + xpos * 16 + ypos * 16 * stridey;
    let mut idxu = buf.get_offset(1) + xpos *  8 + ypos *  8 * strideu;
    let mut idxv = buf.get_offset(2) + xpos *  8 + ypos *  8 * stridev;

    let data = buf.get_data_mut().unwrap();
    let framebuf: &mut [u8] = data.as_mut_slice();

    for j in 0..8 {
        for k in 0..8 {
            let mut v = blk[0][k + j * 8] + i16::from(framebuf[idxy + k]);
            if v < 0 { v = 0; } else if v > 255 { v = 255; }
            framebuf[idxy + k] = v as u8;
        }
        for k in 0..8 {
            let mut v = blk[1][k + j * 8] + i16::from(framebuf[idxy + k + 8]);
            if v < 0 { v = 0; } else if v > 255 { v = 255; }
            framebuf[idxy + k + 8] = v as u8;
        }
        idxy += stridey;
    }
    for j in 0..8 {
        for k in 0..8 {
            let mut v = blk[2][k + j * 8] + i16::from(framebuf[idxy + k]);
            if v < 0 { v = 0; } else if v > 255 { v = 255; }
            framebuf[idxy + k] = v as u8;
        }
        for k in 0..8 {
            let mut v = blk[3][k + j * 8] + i16::from(framebuf[idxy + k + 8]);
            if v < 0 { v = 0; } else if v > 255 { v = 255; }
            framebuf[idxy + k + 8] = v as u8;
        }
        idxy += stridey;
    }

    for j in 0..8 {
        for k in 0..8 {
            let mut v = blk[4][k + j * 8] + i16::from(framebuf[idxu + k]);
            if v < 0 { v = 0; } else if v > 255 { v = 255; }
            framebuf[idxu + k] = v as u8;
        }
        for k in 0..8 {
            let mut v = blk[5][k + j * 8] + i16::from(framebuf[idxv + k]);
            if v < 0 { v = 0; } else if v > 255 { v = 255; }
            framebuf[idxv + k] = v as u8;
        }
        idxu += strideu;
        idxv += stridev;
    }
}

/// Copies block from the picture with pixels beyond the picture borders being replaced with replicated edge pixels.
pub fn edge_emu(src: &NAVideoBuffer<u8>, xpos: isize, ypos: isize, bw: usize, bh: usize, dst: &mut [u8], dstride: usize, comp: usize, align: u8) {
    let stride = src.get_stride(comp);
    let offs   = src.get_offset(comp);
    let (w_, h_) = src.get_dimensions(comp);
    let (hss, vss) = src.get_info().get_format().get_chromaton(comp).unwrap().get_subsampling();
    let data = src.get_data();
    let framebuf: &[u8] = data.as_slice();

    let (w, h) = if align == 0 {
            (w_, h_)
        } else {
            let wa = if align > hss { (1 << (align - hss)) - 1 } else { 0 };
            let ha = if align > vss { (1 << (align - vss)) - 1 } else { 0 };
            ((w_ + wa) & !wa, (h_ + ha) & !ha)
        };

    for y in 0..bh {
        let srcy;
        if (y as isize) + ypos < 0 { srcy = 0; }
        else if (y as isize) + ypos >= (h as isize) { srcy = h - 1; }
        else { srcy = ((y as isize) + ypos) as usize; }

        for x in 0..bw {
            let srcx;
            if (x as isize) + xpos < 0 { srcx = 0; }
            else if (x as isize) + xpos >= (w as isize) { srcx = w - 1; }
            else { srcx = ((x as isize) + xpos) as usize; }
            dst[x + y * dstride] = framebuf[offs + srcx + srcy * stride];
        }
    }
}

/// A generic type for motion interpolation function used by [`copy_blocks`]
///
/// The function expects following parameters:
/// * destination buffer
/// * destination buffer stride
/// * source buffer
/// * source buffer stride
/// * block width
/// * block height
///
/// [`copy_blocks`]: ./fn.copy_blocks.html
pub type BlkInterpFunc = fn(&mut [u8], usize, &[u8], usize, usize, usize);

/// Performs motion compensation on YUV420 macroblock.
///
/// Arguments:
/// * `dx` and `dy` - destination coordinates
/// * `sx` and `sy` - source coordinates
/// * `bw` and `bh` - block dimensions
/// * `preborder` and `postborder` - number of pixels before and after interpolated one used by the interpolation filter.
/// * `mode` - interpolation mode (essentially the index for the `interp` array)
pub fn copy_blocks(dst: &mut NAVideoBuffer<u8>, src: &NAVideoBuffer<u8>,
                   dx: usize, dy: usize, sx: isize, sy: isize, bw: usize, bh: usize,
                   preborder: usize, postborder: usize,
                   mode: usize, interp: &[BlkInterpFunc])
{
    let pre  = if mode != 0 { preborder  as isize } else { 0 };
    let post = if mode != 0 { postborder as isize } else { 0 };
    let (w, h) = src.get_dimensions(0);

    if (sx - pre < 0) || ((sx >> 1) - pre < 0) || (sx + (bw as isize) + post > (w as isize)) ||
       (sy - pre < 0) || ((sy >> 1) - pre < 0) || (sy + (bh as isize) + post > (h as isize)) {
        let ebuf_stride: usize = 32;
        let mut ebuf: Vec<u8> = vec![0; ebuf_stride * (bh + ((pre + post) as usize))];

        for comp in 0..3 {
            let dstride = dst.get_stride(comp);
            let doff    = dst.get_offset(comp);
            let ddta    = dst.get_data_mut().unwrap();
            let dbuf: &mut [u8] = ddta.as_mut_slice();
            let x   = if comp > 0 { dx/2 } else { dx };
            let y   = if comp > 0 { dy/2 } else { dy };
            let sx_ = (if comp > 0 { sx >> 1 } else { sx }) - pre;
            let sy_ = (if comp > 0 { sy >> 1 } else { sy }) - pre;
            let bw_ = (if comp > 0 { bw/2 } else { bw }) + ((pre + post) as usize);
            let bh_ = (if comp > 0 { bh/2 } else { bh }) + ((pre + post) as usize);
            edge_emu(src, sx_ - pre, sy_ - pre, bw_, bh_,
                     ebuf.as_mut_slice(), ebuf_stride, comp, 0);
            let bw_ = if comp > 0 { bw/2 } else { bw };
            let bh_ = if comp > 0 { bh/2 } else { bh };
            (interp[mode])(&mut dbuf[doff + x + y * dstride..], dstride, ebuf.as_slice(), ebuf_stride, bw_, bh_);
        }
    } else {
        for comp in 0..3 {
            let sstride = src.get_stride(comp);
            let soff    = src.get_offset(comp);
            let sdta    = src.get_data();
            let sbuf: &[u8] = sdta.as_slice();
            let dstride = dst.get_stride(comp);
            let doff    = dst.get_offset(comp);
            let ddta    = dst.get_data_mut().unwrap();
            let dbuf: &mut [u8] = ddta.as_mut_slice();
            let x   = if comp > 0 { dx/2 } else { dx };
            let y   = if comp > 0 { dy/2 } else { dy };
            let sx_ = ((if comp > 0 { sx >> 1 } else { sx }) - pre) as usize;
            let sy_ = ((if comp > 0 { sy >> 1 } else { sy }) - pre) as usize;
            let bw_ = if comp > 0 { bw/2 } else { bw };
            let bh_ = if comp > 0 { bh/2 } else { bh };
            (interp[mode])(&mut dbuf[doff + x + y * dstride..], dstride, &sbuf[(soff + sx_ + sy_ * sstride)..], sstride, bw_, bh_);
        }
    }
}

/// Performs motion compensation on arbitrary block on some plane.
///
/// See [`copy_blocks`] for the arguments explanation.
///
/// [`copy_blocks`]: ./fn.copy_blocks.html
pub fn copy_block(dst: &mut NASimpleVideoFrame<u8>, src: NAVideoBufferRef<u8>, comp: usize,
                  dx: usize, dy: usize, mv_x: i16, mv_y: i16, bw: usize, bh: usize,
                  preborder: usize, postborder: usize,
                  mode: usize, interp: &[BlkInterpFunc])
{
    let pre  = if mode != 0 { preborder  as isize } else { 0 };
    let post = if mode != 0 { postborder as isize } else { 0 };
    let (w, h) = src.get_dimensions(comp);
    let sx = (dx as isize) + (mv_x as isize);
    let sy = (dy as isize) + (mv_y as isize);

    if (sx - pre < 0) || (sx + (bw as isize) + post > (w as isize)) ||
       (sy - pre < 0) || (sy + (bh as isize) + post > (h as isize)) {
        let ebuf_stride: usize = 32;
        let mut ebuf: Vec<u8> = vec![0; ebuf_stride * (bh + ((pre + post) as usize))];

        let dstride = dst.stride[comp];
        let doff    = dst.offset[comp];
        let edge = (pre + post) as usize;
        edge_emu(&src, sx - pre, sy - pre, bw + edge, bh + edge,
                 ebuf.as_mut_slice(), ebuf_stride, comp, 0);
        (interp[mode])(&mut dst.data[doff + dx + dy * dstride..], dstride,
                       ebuf.as_slice(), ebuf_stride, bw, bh);
    } else {
        let sstride = src.get_stride(comp);
        let soff    = src.get_offset(comp);
        let sdta    = src.get_data();
        let sbuf: &[u8] = sdta.as_slice();
        let dstride = dst.stride[comp];
        let doff    = dst.offset[comp];
        let saddr = soff + ((sx - pre) as usize) + ((sy - pre) as usize) * sstride;
        (interp[mode])(&mut dst.data[doff + dx + dy * dstride..], dstride,
                       &sbuf[saddr..], sstride, bw, bh);
    }
}

fn hpel_interp00(dst: &mut [u8], dstride: usize, src: &[u8], sstride: usize, bw: usize, bh: usize)
{
    let mut didx = 0;
    let mut sidx = 0;
    for _ in 0..bh {
        dst[didx..][..bw].copy_from_slice(&src[sidx..][..bw]);
        didx += dstride;
        sidx += sstride;
    }
}

fn hpel_interp01(dst: &mut [u8], dstride: usize, src: &[u8], sstride: usize, bw: usize, bh: usize)
{
    let mut didx = 0;
    let mut sidx = 0;
    for _ in 0..bh {
        for x in 0..bw { dst[didx + x] = ((u16::from(src[sidx + x]) + u16::from(src[sidx + x + 1]) + 1) >> 1) as u8; }
        didx += dstride;
        sidx += sstride;
    }
}

fn hpel_interp10(dst: &mut [u8], dstride: usize, src: &[u8], sstride: usize, bw: usize, bh: usize)
{
    let mut didx = 0;
    let mut sidx = 0;
    for _ in 0..bh {
        for x in 0..bw { dst[didx + x] = ((u16::from(src[sidx + x]) + u16::from(src[sidx + x + sstride]) + 1) >> 1) as u8; }
        didx += dstride;
        sidx += sstride;
    }
}

fn hpel_interp11(dst: &mut [u8], dstride: usize, src: &[u8], sstride: usize, bw: usize, bh: usize)
{
    let mut didx = 0;
    let mut sidx = 0;
    for _ in 0..bh {
        for x in 0..bw {
            dst[didx + x] = ((u16::from(src[sidx + x]) +
                              u16::from(src[sidx + x + 1]) +
                              u16::from(src[sidx + x + sstride]) +
                              u16::from(src[sidx + x + sstride + 1]) + 2) >> 2) as u8;
        }
        didx += dstride;
        sidx += sstride;
    }
}

/// Half-pixel interpolation functions.
pub const HALFPEL_INTERP_FUNCS: &[BlkInterpFunc] = &[
        hpel_interp00, hpel_interp01, hpel_interp10, hpel_interp11 ];

