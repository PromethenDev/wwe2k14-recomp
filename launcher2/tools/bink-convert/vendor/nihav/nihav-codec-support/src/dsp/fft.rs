//! FFT and RDFT implementation.
use std::f32::{self, consts};
use std::ops::{Not, Neg, Add, AddAssign, Sub, SubAssign, Mul, MulAssign, Div};
use std::fmt;

/// Complex number.
#[repr(C)]
#[derive(Debug,Clone,Copy,PartialEq)]
pub struct FFTComplex {
    /// Real part of the numner.
    pub re: f32,
    /// Complex part of the number.
    pub im: f32,
}

impl FFTComplex {
    /// Calculates `exp(i * val)`.
    pub fn exp(val: f32) -> Self {
        FFTComplex { re: val.cos(), im: val.sin() }
    }
    /// Returns `-Im + i * Re`.
    pub fn rotate(self) -> Self {
        FFTComplex { re: -self.im, im: self.re }
    }
    /// Multiplies complex number by scalar.
    pub fn scale(self, scale: f32) -> Self {
        FFTComplex { re: self.re * scale, im: self.im * scale }
    }
    /// Returns squared modulus value of the complex number.
    pub fn sq_modulus(self) -> f32 {
        self.re * self.re + self.im * self.im
    }
    /// Returns reciprocal of the complex number.
    pub fn reciprocal(self) -> Self {
        !self.scale(self.sq_modulus())
    }
}

impl Neg for FFTComplex {
    type Output = FFTComplex;
    fn neg(self) -> Self::Output {
        FFTComplex { re: -self.re, im: -self.im }
    }
}

impl Not for FFTComplex {
    type Output = FFTComplex;
    fn not(self) -> Self::Output {
        FFTComplex { re: self.re, im: -self.im }
    }
}

impl Add for FFTComplex {
    type Output = FFTComplex;
    fn add(self, other: Self) -> Self::Output {
        FFTComplex { re: self.re + other.re, im: self.im + other.im }
    }
}

impl AddAssign for FFTComplex {
    fn add_assign(&mut self, other: Self) {
        self.re += other.re;
        self.im += other.im;
    }
}

impl Sub for FFTComplex {
    type Output = FFTComplex;
    fn sub(self, other: Self) -> Self::Output {
        FFTComplex { re: self.re - other.re, im: self.im - other.im }
    }
}

impl SubAssign for FFTComplex {
    fn sub_assign(&mut self, other: Self) {
        self.re -= other.re;
        self.im -= other.im;
    }
}

impl Mul for FFTComplex {
    type Output = FFTComplex;
    fn mul(self, other: Self) -> Self::Output {
        FFTComplex { re: self.re * other.re - self.im * other.im,
                     im: self.im * other.re + self.re * other.im }
    }
}

impl MulAssign for FFTComplex {
    fn mul_assign(&mut self, other: Self) {
        let re = self.re * other.re - self.im * other.im;
        let im = self.im * other.re + self.re * other.im;
        self.re = re;
        self.im = im;
    }
}

impl Div for FFTComplex {
    type Output = FFTComplex;
    fn div(self, other: Self) -> Self::Output {
        self * other.reciprocal()
    }
}

impl fmt::Display for FFTComplex {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        write!(f, "({}, {})", self.re, self.im)
    }
}

/// Complex number with zero value.
pub const FFTC_ZERO: FFTComplex = FFTComplex { re: 0.0, im: 0.0 };

/// Calculates forward or inverse FFT in the straightforward way.
pub fn generic_fft(data: &mut [FFTComplex], forward: bool) {
    let mut tmp = Vec::with_capacity(data.len());
    tmp.resize(data.len(), FFTC_ZERO);
    let base = if forward { -consts::PI * 2.0 / (data.len() as f32) }
               else       {  consts::PI * 2.0 / (data.len() as f32) };
    for k in 0..data.len() {
        let mut sum = FFTC_ZERO;
        for n in 0..data.len() {
            let w = FFTComplex::exp(base * ((n * k) as f32));
            sum += data[n] * w;
        }
        tmp[k] = sum;
    }
    for k in 0..data.len() {
        data[k] = tmp[k];
    }
}

struct FFTData {
    table:      Vec<FFTComplex>,
    tmp:        Vec<FFTComplex>,
    twiddle:    Vec<FFTComplex>,
    size:       usize,
    step:       usize,
    div:        usize,
}

struct FFTGeneric {}

const FFT3_CONST: f32 = 0.86602540378443864677;
const FFT5_CONST1: FFTComplex = FFTComplex { re: 0.80901699437494742410, im: 0.58778525229247312915 };
const FFT5_CONST2: FFTComplex = FFTComplex { re: 0.30901699437494742411, im: 0.95105651629515357211 };

fn twiddle5(a: FFTComplex, b: FFTComplex, c: FFTComplex) -> (FFTComplex, FFTComplex) {
    let re = a.re * c.re;
    let im = a.im * c.re;
    let diffre = b.im * c.im;
    let diffim = b.re * c.im;

    (FFTComplex { re: re - diffre, im: im + diffim }, FFTComplex { re: re + diffre, im: im - diffim })
}

impl FFTGeneric {
    fn new_data(size: usize, forward: bool) -> FFTData {
        let mut table: Vec<FFTComplex> = Vec::with_capacity(size * size);
        table.resize(size * size, FFTC_ZERO);
        let base = consts::PI * 2.0 / (size as f32);
        if forward {
            for n in 0..size {
                for k in 0..size {
                    table[n * size + k] = FFTComplex::exp(-base * ((n * k) as f32));
                }
            }
        } else {
            for n in 0..size {
                for k in 0..size {
                    table[n * size + k] = FFTComplex::exp( base * ((n * k) as f32));
                }
            }
        }
        let mut tmp = Vec::with_capacity(size);
        tmp.resize(size, FFTC_ZERO);
        FFTData { table, tmp, twiddle: Vec::new(), size, step: 0, div: 0 }
    }
    fn fft(tbl: &mut FFTData, size: usize, data: &mut [FFTComplex], step: usize) {
        if size == 3 {
            let s0 = data[step * 0];
            let s1 = data[step * 1];
            let s2 = data[step * 2];
            let t0 = s1 + s2;
            data[step * 0] += t0;
            let t1 = s0 - t0.scale(0.5);
            let t2 = (s2 - s1).rotate().scale(FFT3_CONST);
            data[step * 1] = t1 + t2;
            data[step * 2] = t1 - t2;
            return;
        }
        if size == 5 {
            let s0 = data[step * 0];
            let s1 = data[step * 1];
            let s2 = data[step * 2];
            let s3 = data[step * 3];
            let s4 = data[step * 4];

            let t0 = s1 + s4;
            let t1 = s1 - s4;
            let t2 = s2 + s3;
            let t3 = s2 - s3;
            let (t4, t5) = twiddle5(t0, t1, FFT5_CONST2);
            let (t6, t7) = twiddle5(t2, t3, FFT5_CONST1);
            let (t8, t9) = twiddle5(t0, t1, FFT5_CONST1);
            let (ta, tb) = twiddle5(t2, t3, FFT5_CONST2);

            data[step * 0] = s0 + t0 + t2;
            data[step * 1] = s0 + t5 - t6;
            data[step * 2] = s0 - t8 + ta;
            data[step * 3] = s0 - t9 + tb;
            data[step * 4] = s0 + t4 - t7;
            return;
        }
        for k in 0..tbl.size {
            tbl.tmp[k] = FFTC_ZERO;
            for n in 0..tbl.size {
                tbl.tmp[k] += data[n * step] * tbl.table[k * tbl.size + n];
            }
        }
        for n in 0..tbl.size {
            data[n * step] = tbl.tmp[n];
        }
    }
    fn ifft(tbl: &mut FFTData, size: usize, data: &mut [FFTComplex], step: usize) {
        if size == 3 {
            let s0 = data[step * 0];
            let s1 = data[step * 1];
            let s2 = data[step * 2];
            let t0 = s1 + s2;
            data[step * 0] += t0;
            let t1 = s0 - t0.scale(0.5);
            let t2 = (s2 - s1).rotate().scale(FFT3_CONST);
            data[step * 1] = t1 - t2;
            data[step * 2] = t1 + t2;
            return;
        }
        if size == 5 {
            let s0 = data[step * 0];
            let s1 = data[step * 1];
            let s2 = data[step * 2];
            let s3 = data[step * 3];
            let s4 = data[step * 4];

            let t0 = s1 + s4;
            let t1 = s1 - s4;
            let t2 = s2 + s3;
            let t3 = s2 - s3;
            let (t4, t5) = twiddle5(t0, t1, FFT5_CONST2);
            let (t6, t7) = twiddle5(t2, t3, FFT5_CONST1);
            let (t8, t9) = twiddle5(t0, t1, FFT5_CONST1);
            let (ta, tb) = twiddle5(t2, t3, FFT5_CONST2);

            data[step * 0] = s0 + t0 + t2;
            data[step * 1] = s0 + t4 - t7;
            data[step * 2] = s0 - t9 + tb;
            data[step * 3] = s0 - t8 + ta;
            data[step * 4] = s0 + t5 - t6;
            return;
        }
        Self::fft(tbl, size, data, step);
    }
}

struct FFTSplitRadix {}

impl FFTSplitRadix {
    fn new_data(bits: u8, _forward: bool) -> FFTData {
        let size = 1 << bits;
        let mut table = Vec::with_capacity(size);
        for _ in 0..4 { table.push(FFTC_ZERO); }
        for b in 3..=bits {
            let qsize = (1 << (b - 2)) as usize;
            let base = -consts::PI / ((qsize * 2) as f32);
            for k in 0..qsize {
                table.push(FFTComplex::exp(base * ((k * 1) as f32)));
                table.push(FFTComplex::exp(base * ((k * 3) as f32)));
            }
        }
        FFTData { table, tmp: Vec::new(), twiddle: Vec::new(), size, step: 0, div: 0 }
    }
    fn fft(fftdata: &mut FFTData, bits: u8, data: &mut [FFTComplex]) {
        if bits == 0 { return; }
        if bits == 1 {
            let sum01 = data[0] + data[1];
            let dif01 = data[0] - data[1];
            data[0] = sum01;
            data[1] = dif01;
            return;
        }
        if bits == 2 {
            let sum01 = data[0] + data[2];
            let dif01 = data[0] - data[2];
            let sum23 = data[1] + data[3];
            let dif23 = data[1] - data[3];
            data[0] = sum01 + sum23;
            data[1] = dif01 - dif23.rotate();
            data[2] = sum01 - sum23;
            data[3] = dif01 + dif23.rotate();
            return;
        }
        let qsize = (1 << (bits - 2)) as usize;
        let hsize = (1 << (bits - 1)) as usize;
        let q3size = qsize + hsize;

        Self::fft(fftdata, bits - 1, &mut data[0     ..hsize]);
        Self::fft(fftdata, bits - 2, &mut data[hsize ..q3size]);
        Self::fft(fftdata, bits - 2, &mut data[q3size..]);
        let off = hsize;
        {
            let t3 =  data[0 + hsize] + data[0 + q3size];
            let t4 = (data[0 + hsize] - data[0 + q3size]).rotate();
            let e1 = data[0];
            let e2 = data[0 + qsize];
            data[0]          = e1 + t3;
            data[0 + qsize]  = e2 - t4;
            data[0 + hsize]  = e1 - t3;
            data[0 + q3size] = e2 + t4;
        }
        for k in 1..qsize {
            let t1 = fftdata.table[off + k * 2 + 0] * data[k + hsize];
            let t2 = fftdata.table[off + k * 2 + 1] * data[k + q3size];
            let t3 =  t1 + t2;
            let t4 = (t1 - t2).rotate();
            let e1 = data[k];
            let e2 = data[k + qsize];
            data[k]             = e1 + t3;
            data[k + qsize]     = e2 - t4;
            data[k + hsize]     = e1 - t3;
            data[k + qsize * 3] = e2 + t4;
        }
    }
    fn ifft(fftdata: &mut FFTData, bits: u8, data: &mut [FFTComplex]) {
        if bits == 0 { return; }
        if bits == 1 {
            let sum01 = data[0] + data[1];
            let dif01 = data[0] - data[1];
            data[0] = sum01;
            data[1] = dif01;
            return;
        }
        if bits == 2 {
            let sum01 = data[0] + data[2];
            let dif01 = data[0] - data[2];
            let sum23 = data[1] + data[3];
            let dif23 = data[1] - data[3];
            data[0] = sum01 + sum23;
            data[1] = dif01 + dif23.rotate();
            data[2] = sum01 - sum23;
            data[3] = dif01 - dif23.rotate();
            return;
        }
        let qsize = (1 << (bits - 2)) as usize;
        let hsize = (1 << (bits - 1)) as usize;
        let q3size = qsize + hsize;

        Self::ifft(fftdata, bits - 1, &mut data[0     ..hsize]);
        Self::ifft(fftdata, bits - 2, &mut data[hsize ..q3size]);
        Self::ifft(fftdata, bits - 2, &mut data[q3size..]);
        let off = hsize;
        {
            let t3 =  data[0 + hsize] + data[0 + q3size];
            let t4 = (data[0 + hsize] - data[0 + q3size]).rotate();
            let e1 = data[0];
            let e2 = data[0 + qsize];
            data[0]          = e1 + t3;
            data[0 + qsize]  = e2 + t4;
            data[0 + hsize]  = e1 - t3;
            data[0 + q3size] = e2 - t4;
        }
        for k in 1..qsize {
            let t1 = !fftdata.table[off + k * 2 + 0] * data[k + hsize];
            let t2 = !fftdata.table[off + k * 2 + 1] * data[k + q3size];
            let t3 =  t1 + t2;
            let t4 = (t1 - t2).rotate();
            let e1 = data[k];
            let e2 = data[k + qsize];
            data[k]             = e1 + t3;
            data[k + qsize]     = e2 + t4;
            data[k + hsize]     = e1 - t3;
            data[k + qsize * 3] = e2 - t4;
        }
    }
}

struct FFT15 {}

const FFT15_INSWAP:  [usize; 20] = [ 0, 5, 10, 42, 3, 8, 13, 42, 6, 11, 1, 42, 9, 14, 4, 42, 12, 2, 7, 42 ];
const FFT15_OUTSWAP: [usize; 20] = [ 0, 10, 5, 42, 6, 1, 11, 42, 12, 7, 2, 42, 3, 13, 8, 42, 9, 4, 14, 42 ];

impl FFT15 {
    fn new_data(size: usize, _forward: bool) -> FFTData {
        FFTData { table: Vec::new(), tmp: Vec::new(), twiddle: Vec::new(), size, step: 0, div: 0 }
    }
    fn fft3(dst: &mut [FFTComplex], src: &[FFTComplex], step: usize, n: usize) {
        let s0 = src[0];
        let s1 = src[1];
        let s2 = src[2];

        let t0 = s1 + s2;
        let t1 = s0 - t0.scale(0.5);
        let t2 = (s2 - s1).rotate().scale(FFT3_CONST);

        dst[FFT15_OUTSWAP[n * 4 + 0] * step] = s0 + t0;
        dst[FFT15_OUTSWAP[n * 4 + 1] * step] = t1 + t2;
        dst[FFT15_OUTSWAP[n * 4 + 2] * step] = t1 - t2;
    }
    fn ifft3(dst: &mut [FFTComplex], src: &[FFTComplex], step: usize, n: usize) {
        let s0 = src[0];
        let s1 = src[1];
        let s2 = src[2];

        let t0 = s1 + s2;
        let t1 = s0 - t0.scale(0.5);
        let t2 = (s2 - s1).rotate().scale(FFT3_CONST);

        dst[FFT15_OUTSWAP[n * 4 + 0] * step] = s0 + t0;
        dst[FFT15_OUTSWAP[n * 4 + 1] * step] = t1 - t2;
        dst[FFT15_OUTSWAP[n * 4 + 2] * step] = t1 + t2;
    }
    fn fft5(dst: &mut [FFTComplex], src: &[FFTComplex], step: usize, n: usize) {
        let s0 = src[FFT15_INSWAP[n + 0 * 4] * step];
        let s1 = src[FFT15_INSWAP[n + 1 * 4] * step];
        let s2 = src[FFT15_INSWAP[n + 2 * 4] * step];
        let s3 = src[FFT15_INSWAP[n + 3 * 4] * step];
        let s4 = src[FFT15_INSWAP[n + 4 * 4] * step];

        let t0 = s1 + s4;
        let t1 = s1 - s4;
        let t2 = s2 + s3;
        let t3 = s2 - s3;
        let (t4, t5) = twiddle5(t0, t1, FFT5_CONST2);
        let (t6, t7) = twiddle5(t2, t3, FFT5_CONST1);
        let (t8, t9) = twiddle5(t0, t1, FFT5_CONST1);
        let (ta, tb) = twiddle5(t2, t3, FFT5_CONST2);

        dst[0 * 3] = s0 + t0 + t2;
        dst[1 * 3] = s0 + t5 - t6;
        dst[2 * 3] = s0 - t8 + ta;
        dst[3 * 3] = s0 - t9 + tb;
        dst[4 * 3] = s0 + t4 - t7;
    }
    fn ifft5(dst: &mut [FFTComplex], src: &[FFTComplex], step: usize, n: usize) {
        let s0 = src[FFT15_INSWAP[n + 0 * 4] * step];
        let s1 = src[FFT15_INSWAP[n + 1 * 4] * step];
        let s2 = src[FFT15_INSWAP[n + 2 * 4] * step];
        let s3 = src[FFT15_INSWAP[n + 3 * 4] * step];
        let s4 = src[FFT15_INSWAP[n + 4 * 4] * step];

        let t0 = s1 + s4;
        let t1 = s1 - s4;
        let t2 = s2 + s3;
        let t3 = s2 - s3;
        let (t4, t5) = twiddle5(t0, t1, FFT5_CONST2);
        let (t6, t7) = twiddle5(t2, t3, FFT5_CONST1);
        let (t8, t9) = twiddle5(t0, t1, FFT5_CONST1);
        let (ta, tb) = twiddle5(t2, t3, FFT5_CONST2);

        dst[0 * 3] = s0 + t0 + t2;
        dst[1 * 3] = s0 + t4 - t7;
        dst[2 * 3] = s0 - t9 + tb;
        dst[3 * 3] = s0 - t8 + ta;
        dst[4 * 3] = s0 + t5 - t6;
    }
    fn fft(_fftdata: &mut FFTData, data: &mut [FFTComplex], step: usize) {
        let mut tmp = [FFTC_ZERO; 15];
        for n in 0..3 {
            Self::fft5(&mut tmp[n..], data, step, n);
        }
        for n in 0..5 {
            Self::fft3(data, &tmp[n * 3..][..3], step, n);
        }
    }
    fn ifft(_fftdata: &mut FFTData, data: &mut [FFTComplex], step: usize) {
        let mut tmp = [FFTC_ZERO; 15];
        for n in 0..3 {
            Self::ifft5(&mut tmp[n..], data, step, n);
        }
        for n in 0..5 {
            Self::ifft3(data, &tmp[n * 3..][..3], step, n);
        }
    }
}


enum FFTMode {
    Generic(usize),
    SplitRadix(u8),
    Prime15,
}

impl FFTMode {
    fn permute(&self, perms: &mut [usize]) {
        match *self {
            FFTMode::Generic(_)         => {},
            FFTMode::SplitRadix(bits)   => {
                let div = perms.len() >> bits;
                gen_sr_perms(perms, 1 << bits);
                if div > 1 {
                    for i in 0..(1 << bits) {
                        perms[i] *= div;
                    }
                    for i in 1..div {
                        for j in 0..(1 << bits) {
                            perms[(i << bits) + j] = perms[j] + i;
                        }
                    }
                }
            },
            FFTMode::Prime15            => {},
        };
    }
    fn do_fft(&self, fftdata: &mut FFTData, data: &mut [FFTComplex]) {
        match *self {
            FFTMode::Generic(size)      => FFTGeneric::fft(fftdata, size, data, 1),
            FFTMode::SplitRadix(bits)   => FFTSplitRadix::fft(fftdata, bits, data),
            FFTMode::Prime15            => FFT15::fft(fftdata, data, 1),
        };
    }
    fn do_fft2(&self, fftdata: &mut FFTData, data: &mut [FFTComplex], step: usize) {
        match *self {
            FFTMode::Generic(size)      => FFTGeneric::fft(fftdata, size, data, step),
            FFTMode::SplitRadix(_)      => unreachable!(),
            FFTMode::Prime15            => FFT15::fft(fftdata, data, step),
        };
    }
    fn do_ifft(&self, fftdata: &mut FFTData, data: &mut [FFTComplex]) {
        match *self {
            FFTMode::Generic(size)      => FFTGeneric::ifft(fftdata, size, data, 1),
            FFTMode::SplitRadix(bits)   => FFTSplitRadix::ifft(fftdata, bits, data),
            FFTMode::Prime15            => FFT15::ifft(fftdata, data, 1),
        };
    }
    fn do_ifft2(&self, fftdata: &mut FFTData, data: &mut [FFTComplex], step: usize) {
        match *self {
            FFTMode::Generic(size)      => FFTGeneric::ifft(fftdata, size, data, step),
            FFTMode::SplitRadix(_)      => unreachable!(),
            FFTMode::Prime15            => FFT15::ifft(fftdata, data, step),
        };
    }
    fn get_size(&self) -> usize {
        match *self {
            FFTMode::Generic(size)      => size,
            FFTMode::SplitRadix(bits)   => 1 << bits,
            FFTMode::Prime15            => 15,
        }
    }
}

/// FFT working context.
pub struct FFT {
    perms:  Vec<usize>,
    swaps:  Vec<usize>,
    ffts:   Vec<(FFTMode, FFTData)>,
}

impl FFT {
    /// Calculates Fourier transform.
    pub fn do_fft(&mut self, src: &[FFTComplex], dst: &mut [FFTComplex]) {
        for k in 0..src.len() { dst[k] = src[self.perms[k]]; }
        self.do_fft_core(dst);
    }
    /// Calculates inverse Fourier transform.
    pub fn do_ifft(&mut self, src: &[FFTComplex], dst: &mut [FFTComplex]) {
        for k in 0..src.len() { dst[k] = src[self.perms[k]]; }
        self.do_ifft_core(dst);
    }
    /// Performs inplace FFT.
    pub fn do_fft_inplace(&mut self, data: &mut [FFTComplex]) {
        for idx in 0..self.swaps.len() {
            let nidx = self.swaps[idx];
            if idx != nidx {
                data.swap(nidx, idx);
            }
        }
        self.do_fft_core(data);
    }
    /// Performs inplace inverse FFT.
    pub fn do_ifft_inplace(&mut self, data: &mut [FFTComplex]) {
        for idx in 0..self.swaps.len() {
            let nidx = self.swaps[idx];
            if idx != nidx {
                data.swap(nidx, idx);
            }
        }
        self.do_ifft_core(data);
    }
    fn do_fft_core(&mut self, data: &mut [FFTComplex]) {
        for el in self.ffts.iter_mut() {
            let (mode, ref mut fftdata) = el;
            let bsize = mode.get_size();
            let div = fftdata.div;
            let step = fftdata.step;
            if step == 1 {
                mode.do_fft(fftdata, data);
                for i in 1..div {
                    mode.do_fft(fftdata, &mut data[i * bsize..]);
                }
            } else {
                mode.do_fft2(fftdata, data, div);
                let mut toff = bsize;
                for i in 1..div {
                    for j in 1..bsize {
                        data[i + j * div] *= fftdata.twiddle[toff + j];
                    }
                    mode.do_fft2(fftdata, &mut data[i..], div);
                    toff += bsize;
                }
            }
        }
    }
    fn do_ifft_core(&mut self, data: &mut [FFTComplex]) {
        for el in self.ffts.iter_mut() {
            let (mode, ref mut fftdata) = el;
            let bsize = mode.get_size();
            let div = fftdata.div;
            let step = fftdata.step;
            if step == 1 {
                mode.do_ifft(fftdata, data);
                for i in 1..div {
                    mode.do_ifft(fftdata, &mut data[i * bsize..]);
                }
            } else {
                mode.do_ifft2(fftdata, data, div);
                let mut toff = bsize;
                for i in 1..div {
                    for j in 1..bsize {
                        data[i + j * div] *= fftdata.twiddle[toff + j];
                    }
                    mode.do_ifft2(fftdata, &mut data[i..], div);
                    toff += bsize;
                }
            }
        }
    }
}

/// [`FFT`] context creator.
///
/// [`FFT`]: ./struct.FFT.html
pub struct FFTBuilder {
}

/*fn reverse_bits(inval: u32) -> u32 {
    const REV_TAB: [u8; 16] = [
        0b0000, 0b1000, 0b0100, 0b1100, 0b0010, 0b1010, 0b0110, 0b1110,
        0b0001, 0b1001, 0b0101, 0b1101, 0b0011, 0b1011, 0b0111, 0b1111,
    ];

    let mut ret = 0;
    let mut val = inval;
    for _ in 0..8 {
        ret = (ret << 4) | (REV_TAB[(val & 0xF) as usize] as u32);
        val = val >> 4;
    }
    ret
}

fn swp_idx(idx: usize, bits: u32) -> usize {
    let s = reverse_bits(idx as u32) as usize;
    s >> (32 - bits)
}*/

fn gen_sr_perms(swaps: &mut [usize], size: usize) {
    if size <= 4 { return; }
    let mut evec:  Vec<usize> = Vec::with_capacity(size / 2);
    let mut ovec1: Vec<usize> = Vec::with_capacity(size / 4);
    let mut ovec2: Vec<usize> = Vec::with_capacity(size / 4);
    for k in 0..size/4 {
        evec.push (swaps[k * 4 + 0]);
        ovec1.push(swaps[k * 4 + 1]);
        evec.push (swaps[k * 4 + 2]);
        ovec2.push(swaps[k * 4 + 3]);
    }
    for k in 0..size/2 { swaps[k]            = evec[k]; }
    for k in 0..size/4 { swaps[k +   size/2] = ovec1[k]; }
    for k in 0..size/4 { swaps[k + 3*size/4] = ovec2[k]; }
    gen_sr_perms(&mut swaps[0..size/2],        size/2);
    gen_sr_perms(&mut swaps[size/2..3*size/4], size/4);
    gen_sr_perms(&mut swaps[3*size/4..],       size/4);
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

impl FFTBuilder {
    fn generate_twiddle(data: &mut FFTData, size: usize, cur_size: usize, forward: bool) {
        if size == cur_size { return; }
        data.twiddle = Vec::with_capacity(size);
        let div = size / cur_size;
        let base = if forward { -2.0 * consts::PI / (size as f32) } else { 2.0 * consts::PI / (size as f32) };
        for n in 0..div {
            for k in 0..cur_size {
                data.twiddle.push(FFTComplex::exp(base * ((k * n) as f32)));
            }
        }
    }
    /// Constructs a new `FFT` context.
    pub fn new_fft(size: usize, forward: bool) -> FFT {
        let mut ffts: Vec<(FFTMode, FFTData)> = Vec::with_capacity(1);
        let mut perms: Vec<usize> = Vec::with_capacity(size);
        let mut swaps: Vec<usize> = Vec::with_capacity(size);
        let mut rem_size = size;
        if rem_size.trailing_zeros() > 0 {
            let bits = rem_size.trailing_zeros() as u8;
            let mut data = FFTSplitRadix::new_data(bits, forward);
            Self::generate_twiddle(&mut data, size, 1 << bits, forward);
            data.step = 1;
            data.div = rem_size >> bits;
            ffts.push((FFTMode::SplitRadix(bits), data));
            rem_size >>= bits;
        }
        if (rem_size % 15) == 0 {
            let mut data = FFT15::new_data(size, forward);
            Self::generate_twiddle(&mut data, size, 15, forward);
            data.step = size / rem_size;
            data.div = size / rem_size;
            ffts.push((FFTMode::Prime15, data));
            rem_size /= 15;
        }
        if rem_size > 1 {
            let mut data = FFTGeneric::new_data(rem_size, forward);
            Self::generate_twiddle(&mut data, size, rem_size, forward);
            data.step = size / rem_size;
            data.div  = size / rem_size;
            ffts.push((FFTMode::Generic(rem_size), data));
        }

        for i in 0..size {
            perms.push(i);
        }
        for (mode, _) in ffts.iter().rev() {
            mode.permute(&mut perms);
        }
        gen_swaps_for_perm(&mut swaps, perms.as_slice());

        FFT { perms, swaps, ffts }
    }
}

/// RDFT working context.
pub struct RDFT {
    table:  Vec<FFTComplex>,
    fft:    FFT,
    fwd:    bool,
    size:   usize,
    fwd_fft:    bool,
}

fn crossadd(a: FFTComplex, b: FFTComplex) -> FFTComplex {
    FFTComplex { re: a.re + b.re, im: a.im - b.im }
}

impl RDFT {
    /// Calculates RDFT.
    pub fn do_rdft(&mut self, src: &[FFTComplex], dst: &mut [FFTComplex]) {
        dst.copy_from_slice(src);
        self.do_rdft_inplace(dst);
    }
    /// Calculates inplace RDFT.
    pub fn do_rdft_inplace(&mut self, buf: &mut [FFTComplex]) {
        if !self.fwd {
            for n in 0..self.size/2 {
                let in0 = buf[n + 1];
                let in1 = buf[self.size - n - 1];

                let t0 = crossadd(in0, in1);
                let t1 = FFTComplex { re: in1.im + in0.im, im: in1.re - in0.re };
                let tab = self.table[n];
                let t2 = FFTComplex { re: t1.im * tab.im + t1.re * tab.re, im: t1.im * tab.re - t1.re * tab.im };

                buf[n + 1] = FFTComplex { re: t0.im - t2.im, im: t0.re - t2.re }; // (t0 - t2).conj().rotate()
                buf[self.size - n - 1] = (t0 + t2).rotate();
            }
            let a = buf[0].re;
            let b = buf[0].im;
            buf[0].re = a - b;
            buf[0].im = a + b;
        }
        if self.fwd_fft {
            self.fft.do_fft_inplace(buf);
        } else {
            self.fft.do_ifft_inplace(buf);
        }
        if self.fwd {
            for n in 0..self.size/2 {
                let in0 = buf[n + 1];
                let in1 = buf[self.size - n - 1];

                let t0 = crossadd(in0, in1).scale(0.5);
                let t1 = FFTComplex { re: in0.im + in1.im, im: in0.re - in1.re };
                let t2 = t1 * self.table[n];

                buf[n + 1] = crossadd(t0, t2);
                buf[self.size - n - 1] = FFTComplex { re: t0.re - t2.re, im: -(t0.im + t2.im) };
            }
            let a = buf[0].re;
            let b = buf[0].im;
            buf[0].re = a + b;
            buf[0].im = a - b;
        } else {
            for n in 0..self.size {
                buf[n] = FFTComplex{ re: buf[n].im, im: buf[n].re };
            }
        }
    }
}

/// [`RDFT`] context creator.
///
/// [`RDFT`]: ./struct.FFT.html
pub struct RDFTBuilder {
}

impl RDFTBuilder {
    /// Constructs a new `RDFT` context.
    pub fn new_rdft(size: usize, forward: bool, forward_fft: bool) -> RDFT {
        let mut table: Vec<FFTComplex> = Vec::with_capacity(size / 4);
        let (base, scale) = if forward { (consts::PI / (size as f32), 0.5) } else { (-consts::PI / (size as f32), 1.0) };
        for i in 0..size/2 {
            table.push(FFTComplex::exp(base * ((i + 1) as f32)).scale(scale));
        }
        let fft = FFTBuilder::new_fft(size, forward_fft);
        RDFT { table, fft, size, fwd: forward, fwd_fft: forward_fft }
    }
}


#[cfg(test)]
mod test {
    use super::*;

    fn test_fft(size: usize) {
        println!("testing FFT {}", size);
        let mut fin:   Vec<FFTComplex> = Vec::with_capacity(size);
        let mut fout1: Vec<FFTComplex> = Vec::with_capacity(size);
        let mut fout2: Vec<FFTComplex> = Vec::with_capacity(size);
        fin.resize(size, FFTC_ZERO);
        fout1.resize(size, FFTC_ZERO);
        fout2.resize(size, FFTC_ZERO);
        let mut fft = FFTBuilder::new_fft(size, true);
        let mut seed: u32 = 42;
        for i in 0..fin.len() {
            seed = seed.wrapping_mul(1664525).wrapping_add(1013904223);
            let val = (seed >> 16) as i16;
            fin[i].re = (val as f32) / 256.0;
            seed = seed.wrapping_mul(1664525).wrapping_add(1013904223);
            let val = (seed >> 16) as i16;
            fin[i].im = (val as f32) / 256.0;
        }
        fft.do_fft(&fin, &mut fout1);
        fout2.copy_from_slice(&fin);
        generic_fft(&mut fout2, true);

        for i in 0..fin.len() {
            assert!((fout1[i].re - fout2[i].re).abs() < 1.0);
            assert!((fout1[i].im - fout2[i].im).abs() < 1.0);
        }
        let mut ifft = FFTBuilder::new_fft(size, false);
        ifft.do_ifft_inplace(&mut fout1);
        generic_fft(&mut fout2, false);

        let sc = 1.0 / (size as f32);
        for i in 0..fin.len() {
            assert!((fin[i].re - fout1[i].re * sc).abs() < 1.0);
            assert!((fin[i].im - fout1[i].im * sc).abs() < 1.0);
            assert!((fout1[i].re - fout2[i].re).abs() * sc < 1.0);
            assert!((fout1[i].im - fout2[i].im).abs() * sc < 1.0);
        }
    }

    #[test]
    fn test_ffts() {
        test_fft(3);
        test_fft(5);
        test_fft(16);
        test_fft(15);
        test_fft(60);
        test_fft(256);
        test_fft(240);
    }

    #[test]
    fn test_rdft() {
        let mut fin:   [FFTComplex; 128] = [FFTC_ZERO; 128];
        let mut fout1: [FFTComplex; 128] = [FFTC_ZERO; 128];
        let mut rdft = RDFTBuilder::new_rdft(fin.len(), true, true);
        let mut seed: u32 = 42;
        for i in 0..fin.len() {
            seed = seed.wrapping_mul(1664525).wrapping_add(1013904223);
            let val = (seed >> 16) as i16;
            fin[i].re = (val as f32) / 256.0;
            seed = seed.wrapping_mul(1664525).wrapping_add(1013904223);
            let val = (seed >> 16) as i16;
            fin[i].im = (val as f32) / 256.0;
        }
        rdft.do_rdft(&fin, &mut fout1);
        let mut irdft = RDFTBuilder::new_rdft(fin.len(), false, true);
        irdft.do_rdft_inplace(&mut fout1);

        for i in 0..fin.len() {
            let tst = fout1[i].scale(0.5/(fout1.len() as f32));
            assert!((tst.re - fin[i].re).abs() < 1.0);
            assert!((tst.im - fin[i].im).abs() < 1.0);
        }
    }
}
