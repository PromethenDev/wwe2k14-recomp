use super::*;
use super::kernel::Kernel;
#[cfg(target_arch="x86_64")]
use std::arch::asm;

#[cfg(target_arch="x86_64")]
fn scale2x_sse2(sbuf: &NAVideoBuffer<u8>, dbuf: &mut NAVideoBuffer<u8>) {
    let fmt = sbuf.get_info().get_format();
    let dfmt = dbuf.get_info().get_format();
    let ndcomp = dfmt.get_num_comp();
    let ncomp = fmt.get_num_comp().min(ndcomp);

    for comp in 0..ncomp {
        let istride = sbuf.get_stride(comp);
        let dstride = dbuf.get_stride(comp);
        let (sw, _sh) = sbuf.get_dimensions(comp);
        let (_dw, dh) = dbuf.get_dimensions(comp);
        let ioff = sbuf.get_offset(comp);
        let doff = dbuf.get_offset(comp);
        let src = sbuf.get_data();
        let dst = dbuf.get_data_mut().unwrap();

        unsafe {
            asm!(
                "2:",
                " mov {left}, {width}",
                " mov {line}, {dst}",
                " lea {line2}, [{dst} + {dstride}]",
                " 3:",
                "  movdqa xmm0, [{src}]",
                "  movdqa xmm2, [{src}+16]",
                "  add {src}, 32",
                "  movdqa xmm1, xmm0",
                "  movdqa xmm3, xmm2",
                "  punpcklbw xmm0, xmm0",
                "  punpckhbw xmm1, xmm1",
                "  punpcklbw xmm2, xmm2",
                "  punpckhbw xmm3, xmm3",
                "  movdqa [{line}], xmm0",
                "  movdqa [{line}+16], xmm1",
                "  movdqa [{line}+32], xmm2",
                "  movdqa [{line}+48], xmm3",
                "  add {line}, 64",
                "  movdqa [{line2}], xmm0",
                "  movdqa [{line2}+16], xmm1",
                "  movdqa [{line2}+32], xmm2",
                "  movdqa [{line2}+48], xmm3",
                "  add {line2}, 64",
                "  sub {left}, 32",
                "  jg 3b",
                " add {src}, {istep}",
                " lea {dst}, [{dst} + {dstride} * 2]",
                " sub {height}, 2",
                " jnz 2b",

                dst = inout(reg) dst.as_mut_ptr().add(doff) => _,
                src = inout(reg) src.as_ptr().add(ioff) => _,
                width = in(reg) sw,
                istep = in(reg) istride - sw,
                dstride = in(reg) dstride,
                height = inout(reg) dh => _,
                left = out(reg) _,
                line = out(reg) _,
                line2 = out(reg) _,
                out("xmm0") _,
                out("xmm1") _,
                out("xmm2") _,
                out("xmm3") _,
            );
        }
    }
}

trait ResizeLine<T> {
    fn resize_line(&mut self, src: &[T], src_len: usize, sstep: usize, dst: &mut [T], dst_len: usize, dstep: usize);
}

trait CustomFrom<T> {
    fn cvt_from(val: T) -> Self;
}

impl CustomFrom<usize> for u8 {
    fn cvt_from(val: usize) -> Self { val as u8 }
}
impl CustomFrom<usize> for u16 {
    fn cvt_from(val: usize) -> Self { val as u16 }
}
impl CustomFrom<u8> for usize {
    fn cvt_from(val: u8) -> Self { usize::from(val) }
}
impl CustomFrom<u16> for usize {
    fn cvt_from(val: u16) -> Self { usize::from(val) }
}
impl CustomFrom<f32> for u8 {
    fn cvt_from(val: f32) -> Self { val.clamp(0.0, 255.0) as u8 }
}
impl CustomFrom<f32> for u16 {
    fn cvt_from(val: f32) -> Self { val.clamp(0.0, 65535.0) as u16 }
}
impl CustomFrom<u8> for f32 {
    fn cvt_from(val: u8) -> Self { val as f32 }
}
impl CustomFrom<u16> for f32 {
    fn cvt_from(val: u16) -> Self { val as f32 }
}

trait CustomInto<T:Copy> {
    fn cvt_into(self) -> T;
}

impl<T:Copy, U:Copy> CustomInto<U> for T where U: CustomFrom<T> {
    fn cvt_into(self) -> U { U::cvt_from(self) }
}

#[derive(Clone,Copy,Default)]
struct FracPos {
    ipos:   usize,
    frac:   usize,
}

impl FracPos {
    fn new() -> Self { Self::default() }
    fn add(&mut self, frac: Fraction) {
        self.frac += frac.num;
        while self.frac >= frac.den {
            self.frac -= frac.den;
            self.ipos += 1;
        }
    }
}

#[derive(Clone,Copy,PartialEq)]
struct Fraction {
    num:    usize,
    den:    usize,
}

impl Fraction {
    fn new(a: usize, b: usize) -> Self {
        let cur_gcd = gcd(a, b);
        Self {
            num:    a / cur_gcd,
            den:    b / cur_gcd,
        }
    }
    fn is_one(self) -> bool { self.num == 1 && self.den == 1 }
}

fn gcd(mut a: usize, mut b: usize) -> usize {
    while a != 0 && b != 0 {
        if a > b {
            a -= b;
        } else {
            b -= a;
        }
    }
    a.max(b)
}


#[derive(Clone)]
struct NNResampler {}

impl NNResampler {
    fn new() -> Self { Self{} }
}

impl<T:Copy> ResizeLine<T> for NNResampler {
    fn resize_line(&mut self, src: &[T], src_len: usize, sstep: usize, dst: &mut [T], dst_len: usize, dstep: usize) {
        let frac = Fraction::new(src_len, dst_len);
        let mut pos = FracPos::new();
        for el in dst.chunks_mut(dstep).take(dst_len) {
            el[0] = src[pos.ipos * sstep];
            pos.add(frac);
        }
    }
}

#[derive(Clone)]
struct BilinResize {}

impl BilinResize {
    fn new() -> Self { Self {} }
}

impl<T> ResizeLine<T> for BilinResize
where
    T: Copy+CustomFrom<usize>+CustomInto<usize>
{
    fn resize_line(&mut self, src: &[T], src_len: usize, sstep: usize, dst: &mut [T], dst_len: usize, dstep: usize) {
        let mut pos = FracPos::new();
        let frac = Fraction::new(src_len, dst_len);

        for el in dst.chunks_mut(dstep).take(dst_len) {
            let spos0 = pos.ipos * sstep;
            if pos.frac == 0 {
                el[0] = src[spos0];
            } else {
                let spos1 = (pos.ipos + 1).min(src_len - 1) * sstep;
                let s0: usize = T::cvt_into(src[spos0]);
                let s1: usize = T::cvt_into(src[spos1]);
                el[0] = usize::cvt_into((s0 * (frac.den - pos.frac) + s1 * pos.frac) / frac.den);
            }
            pos.add(frac);
        }
    }
}

#[derive(Clone)]
struct BicubicResize {
    last_frac:  Fraction,
    ccache:     Vec<[f32; 4]>,
}

impl BicubicResize {
    fn new() -> Self {
        Self {
            last_frac:  Fraction::new(1, 1),
            ccache:     Vec::new(),
        }
    }
    fn gen_coeffs(frac: f32) -> [f32; 4] {
        let frac2 = frac * frac;
        let frac3 = frac2 * frac;

        [      -0.5 * frac +       frac2 - 0.5 * frac3,
          1.0              - 2.5 * frac2 + 1.5 * frac3,
                0.5 * frac + 2.0 * frac2 - 1.5 * frac3,
                            -0.5 * frac2 + 0.5 * frac3 ]
    }
    fn gen_cache(den: usize) -> Vec<[f32; 4]> {
        let mut cache = Vec::with_capacity(den);
        for i in 1..den {
            cache.push(Self::gen_coeffs((i as f32) / (den as f32)));
        }
        cache
    }
}

impl<T> ResizeLine<T> for BicubicResize
where
    T: Copy+CustomFrom<f32>+CustomInto<f32>
{
    fn resize_line(&mut self, src: &[T], src_len: usize, sstep: usize, dst: &mut [T], dst_len: usize, dstep: usize) {
        let frac = Fraction::new(src_len, dst_len);
        if frac != self.last_frac {
            self.last_frac = frac;
            self.ccache = Self::gen_cache(frac.den);
        }
        let mut pos = FracPos::new();
        let end = (src_len - 1) * sstep;
        for el in dst.chunks_mut(dstep).take(dst_len) {
            let spos0 = pos.ipos * sstep;
            el[0] = if pos.frac == 0 {
                    src[spos0]
                } else {
                    let spos1 = (spos0 + sstep).min(end);
                    let spos2 = (spos1 + sstep).min(end);
                    let sposm = spos0.saturating_sub(sstep);
                    let sm = T::cvt_into(src[sposm]);
                    let s0 = T::cvt_into(src[spos0]);
                    let s1 = T::cvt_into(src[spos1]);
                    let s2 = T::cvt_into(src[spos2]);

                    let coeffs = &self.ccache[pos.frac - 1];
                    T::cvt_from(sm * coeffs[0] + s0 * coeffs[1] + s1 * coeffs[2] + s2 * coeffs[3])
                };
            pos.add(frac);
        }
    }
}

#[derive(Clone)]
struct LanczosResize {
    last_frac:  Fraction,
    order:      usize,
    ccache:     Vec<Vec<f32>>,
}

impl LanczosResize {
    fn new(order: usize) -> Self {
        Self {
            last_frac:  Fraction::new(1, 1),
            order,
            ccache:     Vec::new(),
        }
    }
    fn get_coeffs(num: usize, den: usize, order: usize, coeffs: &mut [f32]) {
        let norm = std::f32::consts::PI * std::f32::consts::PI;
        let frac = (num as f32) / (den as f32);
        let a = order as f32;
        for (i, coef) in coeffs[..order * 2].iter_mut().enumerate() {
            let x = frac - ((i as f32) + 1.0 - a);
            let fp = std::f32::consts::PI * x;
            *coef = a * fp.sin() * (fp / a).sin() / (norm * x * x);
        }
    }
    fn create_cache(order: usize, den: usize) -> Vec<Vec<f32>> {
        let mut cache = Vec::with_capacity(den);
        for i in 1..den {
            let mut entry = vec![0.0; order * 2];
            Self::get_coeffs(i, den, order, &mut entry);
            cache.push(entry);
        }
        cache
    }
}

impl<T> ResizeLine<T> for LanczosResize
where
    T: Copy+CustomFrom<f32>+CustomInto<f32>
{
    fn resize_line(&mut self, src: &[T], src_len: usize, sstep: usize, dst: &mut [T], dst_len: usize, dstep: usize) {
        let frac = Fraction::new(src_len, dst_len);
        if frac != self.last_frac {
            self.last_frac = frac;
            self.ccache = Self::create_cache(self.order, frac.den);
        }

        let mut pos = FracPos::new();

        for el in dst.chunks_mut(dstep).take(dst_len) {
            if pos.frac == 0 {
                el[0] = src[pos.ipos * sstep];
            } else {
                let coeffs = &self.ccache[pos.frac - 1];
                let mut sum = 0.0;
                for (x, &coef) in coeffs.iter().enumerate() {
                    let cpos = (pos.ipos + 1 + x).saturating_sub(self.order).min(src_len - 1) * sstep;
                    sum += T::cvt_into(src[cpos]) * coef;
                }
                el[0] = T::cvt_from(sum);
            }
            pos.add(frac);
        }
    }
}

macro_rules! scale_loop {
    ($sbuf:expr, $tbuf:expr, $dbuf:expr, $scalers:expr) => {
            let fmt = $sbuf.get_info().get_format();
            let dfmt = $dbuf.get_info().get_format();
            let ndcomp = dfmt.get_num_comp();
            let ncomp = fmt.get_num_comp().min(ndcomp);

            for comp in 0..ncomp {
                let istride = $sbuf.get_stride(comp);
                let dstride = $dbuf.get_stride(comp);
                let (sw, sh) = $sbuf.get_dimensions(comp);
                let (dw, dh) = $dbuf.get_dimensions(comp);
                let ioff = $sbuf.get_offset(comp);
                let doff = $dbuf.get_offset(comp);
                let src = $sbuf.get_data();
                let dst = $dbuf.get_data_mut().unwrap();
                let tstride = (dw + 15) & !15;

                let cur_frac = Fraction::new(sw, dw);
                if !cur_frac.is_one() {
                    let mut idx = $scalers.len();
                    for (i, &(frac, _)) in $scalers.iter().enumerate() {
                        if frac == cur_frac {
                            idx = i;
                            break;
                        }
                    }
                    let resizer = &mut $scalers[idx].1;
                    for (dline, sline) in $tbuf.chunks_mut(tstride).zip(src[ioff..].chunks(istride)).take(sh) {
                        resizer.resize_line(sline, sw, 1, dline, dw, 1);
                    }
                } else {
                    for (dline, sline) in $tbuf.chunks_mut(tstride).zip(src[ioff..].chunks(istride)).take(sh) {
                        dline[..dw].copy_from_slice(&sline[..sw]);
                    }
                }

                let cur_frac = Fraction::new(sh, dh);
                if !cur_frac.is_one() {
                    let mut idx = $scalers.len();
                    for (i, &(frac, _)) in $scalers.iter().enumerate() {
                        if frac == cur_frac {
                            idx = i;
                            break;
                        }
                    }
                    let resizer = &mut $scalers[idx].1;
                    for x in 0..dw {
                        resizer.resize_line(&$tbuf[x..], sh, tstride, &mut dst[doff + x..], dh, dstride);
                    }
                } else {
                    for (dline, sline) in dst[doff..].chunks_mut(dstride).zip($tbuf.chunks(tstride)).take(dh) {
                        dline[..dw].copy_from_slice(&sline[..dw]);
                    }
                }
            }
    };
}

type Resizer<T> = (Fraction, Box<dyn ResizeLine<T>>);

struct Scaler {
    resizers8:  Vec<Resizer<u8>>,
    resizers16: Vec<Resizer<u16>>,
    tmp8:       Vec<u8>,
    tmp16:      Vec<u16>,
    is_nn:      bool,
}

fn set_resizer<T, F>(dst: &mut Vec<Resizer<T>>, new_resizer: F, in_fmt: &ScaleInfo, dest_fmt: &ScaleInfo)
    where F: Fn() -> Box<dyn ResizeLine<T>>
{
    let ncomp = in_fmt.fmt.get_num_comp().min(dest_fmt.fmt.get_num_comp());
    for comp in 0..ncomp {
        if let (Some(sfmt), Some(dfmt)) = (in_fmt.fmt.get_chromaton(comp), dest_fmt.fmt.get_chromaton(comp)) {
            let sw = sfmt.get_width(in_fmt.width);
            let sh = sfmt.get_height(in_fmt.height);
            let dw = dfmt.get_width(dest_fmt.width);
            let dh = dfmt.get_height(dest_fmt.height);
            let frac1 = Fraction::new(sw, dw);
            if !frac1.is_one() {
                let frac1_present = dst.iter().any(|(frac, _)| *frac == frac1);
                if !frac1_present {
                    dst.push((frac1, new_resizer()));
                }
            }
            let frac2 = Fraction::new(sh, dh);
            if !frac2.is_one() {
                let frac2_present = dst.iter().any(|(frac, _)| *frac == frac2);

                if !frac2_present {
                    dst.push((frac2, new_resizer()));
                }
            }
        }
    }
}

impl Scaler {
    fn new() -> Self {
        Self {
            resizers8:  Vec::new(),
            resizers16: Vec::new(),
            tmp8:       Vec::new(),
            tmp16:      Vec::new(),
            is_nn:      false,
        }
    }
}

impl Kernel for Scaler {
    fn init(&mut self, in_fmt: &ScaleInfo, dest_fmt: &ScaleInfo, options: &[(String, String)]) -> ScaleResult<NABufferType> {
        let is16 = in_fmt.fmt.get_max_depth() > 8;
        for (name, value) in options.iter() {
            if name.as_str() == "scaler" {
                match value.as_str() {
                    "nn" => {
                        if !is16 {
                            set_resizer(&mut self.resizers8, || Box::new(NNResampler::new()), in_fmt, dest_fmt);
                            self.is_nn = true;
                        } else {
                            set_resizer(&mut self.resizers16, || Box::new(NNResampler::new()), in_fmt, dest_fmt);
                        }
                    },
                    "bilin" => {
                        if !is16 {
                            set_resizer(&mut self.resizers8, || Box::new(BilinResize::new()), in_fmt, dest_fmt);
                        } else {
                            set_resizer(&mut self.resizers16, || Box::new(BilinResize::new()), in_fmt, dest_fmt);
                        }
                    },
                    "bicubic" => {
                        if !is16 {
                            set_resizer(&mut self.resizers8, || Box::new(BicubicResize::new()), in_fmt, dest_fmt);
                        } else {
                            set_resizer(&mut self.resizers16, || Box::new(BicubicResize::new()), in_fmt, dest_fmt);
                        }
                    },
                    _ => {},
                };
                if value.as_str().starts_with("lanczos") {
                    let tail = &value[7..];

                    let mut filt_len = if let Ok(val) = tail.parse::<usize>() {
                            if (2..=16).contains(&val) {
                                val
                            } else {
                                0
                            }
                        } else {
                            0
                        };
                    if filt_len == 0 {
                        filt_len = 3;
                    }
                    if !is16 {
                        set_resizer(&mut self.resizers8, || Box::new(LanczosResize::new(filt_len)), in_fmt, dest_fmt);
                    } else {
                        set_resizer(&mut self.resizers16, || Box::new(LanczosResize::new(filt_len)), in_fmt, dest_fmt);
                    }
                }
            }
        }
        if !is16 && self.resizers8.is_empty() {
            set_resizer(&mut self.resizers8, || Box::new(NNResampler::new()), in_fmt, dest_fmt);
            self.is_nn = true;
        }
        if is16 && self.resizers16.is_empty() {
            set_resizer(&mut self.resizers16, || Box::new(NNResampler::new()), in_fmt, dest_fmt);
        }

        if in_fmt.width * 2 != dest_fmt.width || in_fmt.height * 2 != dest_fmt.height {
            self.is_nn = false;
        }

        let mut max_size = 0;
        let ncomp = in_fmt.fmt.get_num_comp().min(dest_fmt.fmt.get_num_comp());
        for comp in 0..ncomp {
            if let (Some(sfmt), Some(dfmt)) = (in_fmt.fmt.get_chromaton(comp), dest_fmt.fmt.get_chromaton(comp)) {
                let sw = sfmt.get_width(in_fmt.width);
                let sh = sfmt.get_height(in_fmt.height);
                let dw = dfmt.get_width(dest_fmt.width);
                let dh = dfmt.get_height(dest_fmt.height);
                if sw * 2 != dw || sh * 2 != dh {
                    self.is_nn = false;
                }
                let tmp_size = sh * ((dw + 15) & !15);
                max_size = max_size.max(tmp_size);
            }
        }
        if !is16 {
            self.tmp8.resize(max_size, 0);
        } else {
            self.tmp16.resize(max_size, 0);
        }

        let res = alloc_video_buffer(NAVideoInfo::new(dest_fmt.width, dest_fmt.height, false, in_fmt.fmt), 4);
        if res.is_err() { return Err(ScaleError::AllocError); }
        Ok(res.unwrap())
    }
    fn process(&mut self, pic_in: &NABufferType, pic_out: &mut NABufferType) {
        if let (Some(ref sbuf), Some(ref mut dbuf)) = (pic_in.get_vbuf(), pic_out.get_vbuf()) {
            #[cfg(target_arch="x86_64")]
            if self.is_nn {
                let info = sbuf.get_info();
                let mut aligned = true;
                let width  = info.width;
                let height = info.height;
                for comp in info.format.comp_info.iter().flatten() {
                    if ((width >> comp.h_ss) & 0x1F) != 0 || ((height >> comp.v_ss) & 1) != 0 {
                        aligned = false;
                        break;
                    }
                }
                if aligned {
                    scale2x_sse2(sbuf, dbuf);
                    return;
                }
            }
            scale_loop!(sbuf, self.tmp8, dbuf, self.resizers8);
        } else if let (Some(ref sbuf), Some(ref mut dbuf)) = (pic_in.get_vbuf16(), pic_out.get_vbuf16()) {
            scale_loop!(sbuf, self.tmp16, dbuf, self.resizers16);
        } else {
            unreachable!();
        }
    }
}

pub fn create_scale() -> Box<dyn Kernel> {
    Box::new(Scaler::new())
}

