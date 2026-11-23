use nihav_codec_support::codecs::{MV, ZERO_MV};

pub fn pix_dist(a: u8, b: u8) -> u32 {
    ((i32::from(a) - (i32::from(b))) * (i32::from(a) - (i32::from(b)))) as u32
}

pub fn calc_diff(src1: &[u8], stride1: usize, src2: &[u8], stride2: usize) -> u32 {
    src1.chunks(stride1).zip(src2.chunks(stride2)).take(8).fold(0u32,
        |acc, (line1, line2)| acc + line1[..8].iter().zip(line2.iter()).fold(0u32,
            |acc2, (&a, &b)| acc2 + pix_dist(a, b)))
}

const DIA_LARGE: [MV; 4] = [MV{x: 2, y: 0}, MV{x: 0, y: 2}, MV{x: -2, y: 0}, MV{x: 0, y: -2}];
const DIA_SMALL: [MV; 4] = [MV{x: 1, y: 0}, MV{x: 0, y: 1}, MV{x: -1, y: 0}, MV{x: 0, y: -1}];
fn check_mv(x: usize, y: usize, width: usize, height: usize, mv: MV) -> bool {
    let xpos = (x as isize) + isize::from(mv.x);
    let ypos = (y as isize) + isize::from(mv.y);

    mv.x.abs() <= 15 && mv.y.abs() <= 15 &&
    xpos >= 0 && (xpos + 8 <= (width as isize)) &&
    ypos >= 0 && (ypos + 8 <= (height as isize))
}

#[allow(clippy::too_many_arguments)]
pub fn mv_search(src: &[u8], stride: usize, width: usize, height: usize,
             x: usize, y: usize, skip_diff: u32,
             ref_blk: &[u8; 64], tmp: &mut [u8; 64]) -> (MV, u32) {
    let mut best_diff = skip_diff;
    let mut best_mv = ZERO_MV;
    loop {
        let last_mv = best_mv;
        for &off_mv in DIA_LARGE.iter() {
            let mv = best_mv + off_mv;
            if !check_mv(x, y, width, height, mv) {
                continue;
            }
            get_block(src, stride, x, y, mv, tmp);
            let diff = calc_diff(ref_blk, 8, tmp, 8);
            if diff < best_diff {
                best_diff = diff;
                best_mv = mv;
            }
        }
        if best_mv == last_mv {
            break;
        }
    }
    loop {
        let last_mv = best_mv;
        for &off_mv in DIA_SMALL.iter() {
            let mv = best_mv + off_mv;
            if !check_mv(x, y, width, height, mv) {
                continue;
            }
            get_block(src, stride, x, y, mv, tmp);
            let diff = calc_diff(ref_blk, 8, tmp, 8);
            if diff < best_diff {
                best_diff = diff;
                best_mv = mv;
            }
        }
        if best_mv == last_mv {
            break;
        }
    }
    (best_mv, best_diff)
}

pub fn get_block(src: &[u8], stride: usize, x: usize, y: usize, mv: MV, dst: &mut [u8; 64]) {
    let pos = (x as isize + isize::from(mv.x) + (y as isize + isize::from(mv.y)) * (stride as isize)) as usize;
    for (dline, sline) in dst.chunks_exact_mut(8).zip(src[pos..].chunks(stride)) {
        dline.copy_from_slice(&sline[..8]);
    }
}

pub fn put_block(dst: &mut [u8], dstride: usize, cur_blk: &[u8; 64]) {
    for (dline, sline) in dst.chunks_mut(dstride).zip(cur_blk.chunks_exact(8)) {
        dline[..8].copy_from_slice(sline);
    }
}
