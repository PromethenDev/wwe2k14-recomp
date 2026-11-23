//! Discrete 1-D cosine and sine transforms.
use std::f32::consts;

/// A list of DCT and DST modes.
#[allow(non_camel_case_types)]
#[derive(Clone,Copy,Debug,PartialEq)]
pub enum DCTMode {
    DCT_I,
    DCT_II,
    DCT_III,
    DCT_IV,
    DST_I,
    DST_II,
    DST_III,
    DST_IV,
}

/// DCT/DST working context.
#[allow(dead_code)]
pub struct DCT {
    tmp:        Vec<f32>,
    tab:        Vec<f32>,
    swaps:      Vec<usize>,
    perms:      Vec<usize>,
    mode:       DCTMode,
    size:       usize,
    is_pow2:    bool,
    perm_tab:   Vec<usize>,
}

impl DCT {
    /// Constructs a new context for the selected DCT or DST operation.
    pub fn new(mode: DCTMode, size: usize) -> Self {
        let bits = 31 - (size as u32).leading_zeros();
        let is_pow2 = (size & (size - 1)) == 0;
        let mut tmp: Vec<f32>;
        let mut swaps: Vec<usize> = Vec::new();
        let mut perms: Vec<usize>;
        let mut perm_tab: Vec<usize>;
        tmp = Vec::with_capacity(size);
        tmp.resize(size, 0.0);
        if !is_pow2 {
            perms = Vec::new();
            perm_tab = Vec::new();
        } else {
            perms = Vec::with_capacity(size);
            for i in 0..size { perms.push(swp_idx(i, bits)); }
            gen_swaps_for_perm(&mut swaps, &perms);

            perm_tab = Vec::with_capacity(size);
            perm_tab.push(0); // padding
            perm_tab.push(0); // size = 1
            perm_tab.push(0); // size = 2
            perm_tab.push(1);
            for blen in 2..=bits {
                let ssize = 1 << blen;
                for i in 0..ssize { perm_tab.push(swp_idx(i, blen)); }
            }
        }
        let mut tab: Vec<f32>;
        match mode {
            DCTMode::DCT_II |
            DCTMode::DST_II |
            DCTMode::DCT_III |
            DCTMode::DST_III |
            DCTMode::DCT_IV => {
                    tab = Vec::with_capacity(size * 2);
                    tab.push(1.0); // padding
                    tab.push(0.0);
                    tab.push((consts::PI / 8.0).sin()); // size = 1
                    tab.push((consts::PI / 8.0).cos());
                    if bits > 1 {
                        for blen in 1..=bits {
                            let tsize = 1 << blen;
                            let base = consts::PI / ((tsize * 8) as f32);
                            for i in 0..tsize {
                                let phi = ((i * 2 + 1) as f32) * base;
                                tab.push(phi.sin());
                                tab.push(phi.cos());
                            }
                        }
                    }
                },
/*            DCTMode::DST_IV => {
                },*/
            _ => { tab = Vec::new(); },
        };

        Self { tmp, tab, mode, size, swaps, perms, is_pow2, perm_tab }
    }
    fn can_do_fast(&mut self) -> bool {
        if !self.is_pow2 { return false; }
        match self.mode {
            DCTMode::DCT_I | DCTMode::DST_I | DCTMode::DST_IV => false,
            _ => true,
        }
    }
    fn inplace_fast_dct(&mut self, dst: &mut [f32]) {
        match self.mode {
            DCTMode::DCT_II  => {
                    dct_II_inplace(dst, self.size, 1, &self.tab, &self.perm_tab);
                },
            DCTMode::DST_II  => {
                    dst_II_inplace(dst, self.size, 1, &self.tab, &self.perm_tab);
                },
            DCTMode::DCT_III => {
                    dct_III_inplace(dst, self.size, 1, &self.tab, &self.perm_tab);
                },
            DCTMode::DST_III => {
                    dst_III_inplace(dst, self.size, 1, &self.tab, &self.perm_tab);
                },
            DCTMode::DCT_IV  => {
                    dct_IV_inplace(dst, self.size, 1, &self.tab, &self.perm_tab);
                },
            _ => unreachable!(),
        };
    }
    /// Performs DCT/DST.
    pub fn do_dct(&mut self, src: &[f32], dst: &mut [f32]) {
        if self.can_do_fast() {
            for (i, ni) in self.perms.iter().enumerate() { dst[i] = src[*ni]; }
            self.inplace_fast_dct(dst);
        } else {
            do_ref_dct(self.mode, src, dst, self.size);
        }
    }
    /// Performs inplace DCT/DST.
    pub fn do_dct_inplace(&mut self, buf: &mut [f32]) {
        if self.can_do_fast() {
            swap_buf(buf, &self.swaps);
            self.inplace_fast_dct(buf);
        } else {
            self.tmp.copy_from_slice(&buf[0..self.size]);
            do_ref_dct(self.mode, &self.tmp, buf, self.size);
        }
    }
    /// Returns the scale for output normalisation.
    pub fn get_scale(&self) -> f32 {
        let fsize = self.size as f32;
        match self.mode {
            DCTMode::DCT_I  => 2.0 / (fsize - 1.0),
            DCTMode::DST_I  => 2.0 / (fsize + 1.0),
            DCTMode::DCT_II => 1.0,
            DCTMode::DCT_III=> 1.0,
            DCTMode::DST_II => 1.0,
            DCTMode::DST_III=> 1.0,
            DCTMode::DCT_IV => 1.0,
            _               => 2.0 / fsize,
        }
    }
}

fn reverse_bits(inval: u32) -> u32 {
    const REV_TAB: [u8; 16] = [
        0b0000, 0b1000, 0b0100, 0b1100, 0b0010, 0b1010, 0b0110, 0b1110,
        0b0001, 0b1001, 0b0101, 0b1101, 0b0011, 0b1011, 0b0111, 0b1111,
    ];

    let mut ret = 0;
    let mut val = inval;
    for _ in 0..8 {
        ret = (ret << 4) | u32::from(REV_TAB[(val & 0xF) as usize]);
        val >>= 4;
    }
    ret
}

fn swp_idx(idx: usize, bits: u32) -> usize {
    let s = reverse_bits(idx as u32) as usize;
    s >> (32 - bits)
}

fn gen_swaps_for_perm(swaps: &mut Vec<usize>, perms: &[usize]) {
    let mut idx_arr: Vec<usize> = Vec::with_capacity(perms.len());
    for i in 0..perms.len() { idx_arr.push(i); }
    let mut run_size = 0;
    let mut run_pos  = 0;
    for idx in 0..perms.len() {
        if perms[idx] == idx_arr[idx] {
            if run_size == 0 { run_pos = idx; }
            run_size += 1;
        } else {
            for i in 0..run_size {
                swaps.push(run_pos + i);
            }
            run_size = 0;
            let mut spos = idx + 1;
            while idx_arr[spos] != perms[idx] { spos += 1; }
            idx_arr[spos] = idx_arr[idx];
            idx_arr[idx]  = perms[idx];
            swaps.push(spos);
        }
    }
}

fn swap_buf(buf: &mut [f32], swaps: &[usize]) {
    for (idx, nidx) in swaps.iter().enumerate() {
        if idx != *nidx {
            buf.swap(*nidx, idx);
        }
    }
}

fn do_ref_dct(mode: DCTMode, src: &[f32], dst: &mut [f32], size: usize) {
    match mode {
        DCTMode::DCT_I   => dct_I_ref(src, dst, size),
        DCTMode::DST_I   => dst_I_ref(src, dst, size),
        DCTMode::DCT_II  => dct_II_ref(src, dst, size),
        DCTMode::DST_II  => dst_II_ref(src, dst, size),
        DCTMode::DCT_III => dct_III_ref(src, dst, size),
        DCTMode::DST_III => dst_III_ref(src, dst, size),
        DCTMode::DCT_IV  => dct_IV_ref(src, dst, size),
        DCTMode::DST_IV  => dst_IV_ref(src, dst, size),
    };
}

#[allow(non_snake_case)]
fn dct_I_ref(src: &[f32], dst: &mut [f32], size: usize) {
    let base = consts::PI / ((size - 1) as f32);
    for k in 0..size {
        let mut sum = (src[0] + (if (k & 1) != 0 { -src[size - 1] } else { src[size - 1] })) * 0.5;
        for n in 1..size-1 {
            sum += src[n] * (base * ((n * k) as f32)).cos();
        }
        dst[k] = sum;
    }
}

#[allow(non_snake_case)]
fn dst_I_ref(src: &[f32], dst: &mut [f32], size: usize) {
    let base = consts::PI / ((size + 1) as f32);
    for k in 0..size {
        let mut sum = 0.0;
        for n in 0..size {
            sum += src[n] * (base * (((n + 1) * (k + 1)) as f32)).sin();
        }
        dst[k] = sum;
    }
}

#[allow(non_snake_case)]
fn dct_II_ref(src: &[f32], dst: &mut [f32], size: usize) {
    let base = consts::PI / (size as f32);
    for k in 0..size {
        let mut sum = 0.0;
        for n in 0..size {
            sum += src[n] * (base * ((n as f32) + 0.5) * (k as f32)).cos();
        }
        dst[k] = sum * (if k == 0 { (1.0 / (size as f32)).sqrt() } else { (2.0 / (size as f32)).sqrt() });
    }
}

#[allow(non_snake_case)]
fn dst_II_ref(src: &[f32], dst: &mut [f32], size: usize) {
    let base = consts::PI / (size as f32);
    for k in 0..size {
        let mut sum = 0.0;
        let kmod = (k + 1) as f32;
        for n in 0..size {
            sum += src[n] * (base * ((n as f32) + 0.5) * kmod).sin();
        }
        dst[k] = sum * (2.0 / (size as f32)).sqrt();
    }
    dst[size - 1] /= consts::SQRT_2;
}

#[allow(non_snake_case)]
fn dct_III_ref(src: &[f32], dst: &mut [f32], size: usize) {
    let base = consts::PI / (size as f32);
    for k in 0..size {
        let mut sum = src[0] / consts::SQRT_2;
        let kmod = (k as f32) + 0.5;
        for n in 1..size {
            sum += src[n] * (base * (n as f32) * kmod).cos();
        }
        dst[k] = sum * (2.0 / (size as f32)).sqrt();
    }
}

#[allow(non_snake_case)]
fn dst_III_ref(src: &[f32], dst: &mut [f32], size: usize) {
    let base = consts::PI / (size as f32);
    for k in 0..size {
        let mut sum = 0.0;
        let kmod = (k as f32) + 0.5;
        for n in 0..size-1 {
            sum += src[n] * (base * ((n + 1) as f32) * kmod).sin();
        }
        sum += src[size - 1] / consts::SQRT_2 * (base * (size as f32) * kmod).sin();
        dst[k] = sum * (2.0 / (size as f32)).sqrt();
    }
}

#[allow(non_snake_case)]
fn dct_IV_ref(src: &[f32], dst: &mut [f32], size: usize) {
    let base = consts::PI / (size as f32);
    for k in 0..size {
        let mut sum = 0.0;
        let kmod = (k as f32) + 0.5;
        for n in 0..size {
            sum += src[n] * (base * ((n as f32) + 0.5) * kmod).cos();
        }
        dst[k] = sum;
    }
}

#[allow(non_snake_case)]
fn dst_IV_ref(src: &[f32], dst: &mut [f32], size: usize) {
    let base = consts::PI / (size as f32);
    for k in 0..size {
        let mut sum = 0.0;
        let kmod = (k as f32) + 0.5;
        for n in 0..size {
            sum += src[n] * (base * ((n as f32) + 0.5) * kmod).sin();
        }
        dst[k] = sum;
    }
}

const DCT_II_C0: f32 = 0.65328148243818826393; // cos(1*PI/8) / sqrt(2)
const DCT_II_C1: f32 = 0.27059805007309849220; // cos(3*PI/8) / sqrt(2)

#[allow(non_snake_case)]
fn dct_II_inplace(buf: &mut [f32], size: usize, step: usize, tab: &[f32], perm_tab: &[usize]) {
    match size {
        0 | 1 => {},
        2 => {
                let i0 = buf[0];
                let i1 = buf[step];
                buf[0]    = (i0 + i1) / consts::SQRT_2;
                buf[step] = (i0 - i1) / consts::SQRT_2;
            },
        4 => {
                let i0 = buf[0 * step];
                let i1 = buf[2 * step];
                let i2 = buf[1 * step];
                let i3 = buf[3 * step];
                let t0 = (i0 + i3) * 0.5;
                let t1 = (i1 + i2) * 0.5;
                buf[0 * step] = t0 + t1;
                buf[2 * step] = t0 - t1;
                let t0 = i0 - i3;
                let t1 = i1 - i2;
                buf[1 * step] = DCT_II_C0 * t0 + DCT_II_C1 * t1;
                buf[3 * step] = DCT_II_C1 * t0 - DCT_II_C0 * t1;
            },
        _ => {
                let hsize = size >> 1;
                for i in 0..hsize {
                    let i0 = buf[i * step];
                    let i1 = buf[(size - 1 - i) * step];
                    if (i & 1) == 0 {
                        buf[i * step]              = (i0 + i1) / consts::SQRT_2;
                        buf[(size - 1 - i) * step] = (i0 - i1) / consts::SQRT_2;
                    } else {
                        buf[i * step]              = (i1 - i0) / consts::SQRT_2;
                        buf[(size - 1 - i) * step] = (i1 + i0) / consts::SQRT_2;
                    }
                }
                dct_II_inplace(buf,                    hsize, step * 2, tab, perm_tab);
                dct_II_part2_inplace(&mut buf[step..], hsize, step * 2, tab, perm_tab);
            },
    };
}

#[allow(non_snake_case)]
fn dct_II_part2_inplace(buf: &mut [f32], size: usize, step: usize, tab: &[f32], perm_tab: &[usize]) {
    let hsize = size >> 1;
// todo optimise for size = 4
    for i in 0..hsize {
        let i0 = buf[perm_tab[size + i] * step];
        let i1 = buf[perm_tab[size + size - i - 1] * step];
        let c0 = tab[size + i * 2 + 0];
        let c1 = tab[size + i * 2 + 1];
        buf[perm_tab[size + i] * step]             = c0 * i0 + c1 * i1;
        buf[perm_tab[size + size - i - 1] * step]  = c0 * i1 - c1 * i0;
    }

    dct_II_inplace(buf,              hsize, step * 2, tab, perm_tab);
    dst_II_inplace(&mut buf[step..], hsize, step * 2, tab, perm_tab);

    buf[(size - 1) * step] = -buf[(size - 1) * step];
    for i in 1..hsize {
        let (i0, i1) = if (i & 1) == 0 {
                (buf[i * step * 2], -buf[i * step * 2 - step])
            } else {
                (buf[i * step * 2],  buf[i * step * 2 - step])
            };
        buf[i * step * 2 - step] = (i0 + i1) / consts::SQRT_2;
        buf[i * step * 2]        = (i0 - i1) / consts::SQRT_2;
    }
}

#[allow(non_snake_case)]
fn dst_II_inplace(buf: &mut [f32], size: usize, step: usize, tab: &[f32], perm_tab: &[usize]) {
    if size <= 1 { return; }
    let hsize = size >> 1;
    for i in hsize..size { buf[i * step] = -buf[i * step]; }
    dct_II_inplace(buf, size, step, tab, perm_tab);
    for i in 0..hsize {
        let idx0 = i * step;
        let idx1 = (size - 1 - i) * step;
        buf.swap(idx0, idx1);
    }
}

#[allow(non_snake_case)]
fn dct_III_inplace(buf: &mut [f32], size: usize, step: usize, tab: &[f32], perm_tab: &[usize]) {
    if size <= 1 { return; }
    let hsize = size >> 1;
    dct_III_inplace(buf,                   hsize, step, tab, perm_tab);
    dct_IV_inplace(&mut buf[step*hsize..], hsize, step, tab, perm_tab);
    for i in 0..(size >> 2) {
        buf.swap((size - 1 - i) * step, (hsize + i) * step);
    }
    for i in 0..hsize {
        let i0 = buf[i * step] / consts::SQRT_2;
        let i1 = buf[(size-i-1) * step] / consts::SQRT_2;
        buf[i * step] = i0 + i1;
        buf[(size-i-1) * step] = i0 - i1;
    }
}

#[allow(non_snake_case)]
fn dst_III_inplace(buf: &mut [f32], size: usize, step: usize, tab: &[f32], perm_tab: &[usize]) {
    if size <= 1 { return; }
    let hsize = size >> 1;
    for i in 0..hsize {
        let idx0 = i * step;
        let idx1 = (size - 1 - i) * step;
        buf.swap(idx0, idx1);
    }
    dct_III_inplace(buf, size, step, tab, perm_tab);
    for i in 0..hsize { buf[i * 2 * step + step] = -buf[i * 2 * step + step]; }
}

#[allow(non_snake_case)]
fn dct_IV_inplace(buf: &mut [f32], size: usize, step: usize, tab: &[f32], perm_tab: &[usize]) {
    if size <= 1 { return; }
    let hsize = size >> 1;

    for i in 0..hsize {
        let idx0 = perm_tab[size + i];
        let idx1 = size - 1 - idx0;
        let i0 = buf[idx0 * step];
        let i1 = buf[idx1 * step];
        let c0 = tab[size + i * 2 + 1];
        let c1 = tab[size + i * 2 + 0];
        buf[idx0 * step] = c0 * i0 + c1 * i1;
        buf[idx1 * step] = c0 * i1 - c1 * i0;
    }
    for i in (hsize+1..size).step_by(2) {
        buf[i] = -buf[i];
    }
    dct_II_inplace(buf,              hsize, step * 2, tab, perm_tab);
    dct_II_inplace(&mut buf[step..], hsize, step * 2, tab, perm_tab);
    for i in 0..(size >> 2) {
        buf.swap((size - 1 - i * 2) * step, (i * 2 + 1) * step);
    }
    for i in (3..size).step_by(4) {
        buf[i] = -buf[i];
    }
    buf[0]                 *=  consts::SQRT_2;
    buf[(size - 1) * step] *= -consts::SQRT_2;
    for i in 0..hsize-1 {
        let i0 = buf[(i * 2 + 2) * step];
        let i1 = buf[(i * 2 + 1) * step];
        buf[(i * 2 + 2) * step] = i0 + i1;
        buf[(i * 2 + 1) * step] = i0 - i1;
    }
    for i in 0..size {
        buf[i * step] /= consts::SQRT_2;
    }
}

#[cfg(test)]
mod test {
    use super::*;

    fn test_pair(mode: DCTMode, invmode: DCTMode, size: usize) {
        println!("testing {:?} -> {:?}", mode, invmode);
        let mut fin: Vec<f32> = Vec::with_capacity(size);
        let mut out: Vec<f32> = Vec::with_capacity(size);
        out.resize(size, 0.0);
        let mut seed: u32 = 42;
        for _ in 0..size {
            seed = seed.wrapping_mul(1664525).wrapping_add(1013904223);
            let val = (seed >> 16) as i16;
            fin.push((val as f32) / 256.0);
        }
        let mut dct = DCT::new(mode, size);
        dct.do_dct(&fin, &mut out);
        let mut dct = DCT::new(invmode, size);
        dct.do_dct_inplace(&mut out);

        let scale = dct.get_scale();
        for i in 0..fin.len() {
            assert!((fin[i] - out[i]*scale).abs() < 1.0e-2);
        }
    }
    #[test]
    fn test_dct() {
        test_pair(DCTMode::DCT_I,   DCTMode::DCT_I,   32);
        test_pair(DCTMode::DST_I,   DCTMode::DST_I,   32);
        test_pair(DCTMode::DCT_II,  DCTMode::DCT_III, 32);
        test_pair(DCTMode::DST_II,  DCTMode::DST_III, 32);
        test_pair(DCTMode::DCT_III, DCTMode::DCT_II,  32);
        test_pair(DCTMode::DST_III, DCTMode::DST_II,  32);
        test_pair(DCTMode::DCT_IV,  DCTMode::DCT_IV,  32);
        test_pair(DCTMode::DST_IV,  DCTMode::DST_IV,  32);
    }
}
