//! Modified Discrete Cosine transform functionality.
use std::f32::consts;
use super::fft::*;

/// IMDCT working context.
pub struct IMDCT {
    twiddle:    Vec<FFTComplex>,
    fft:        FFT,
    size:       usize,
    z:          Vec<FFTComplex>,
}

/*
fn imdct(src: &[f32], dst: &mut [f32], length: usize) {
    for n in 0..length*2 {
        dst[n] = 0.0;
        for k in 0..length {
            dst[n] += src[k] * (consts::PI / (length as f32) * ((n as f32) + 0.5 + ((length/2) as f32)) * ((k as f32) + 0.5)).cos();
        }
    }
}*/

impl IMDCT {
    /// Constructs a new instance of `IMDCT` context.
    pub fn new(size: usize, scaledown: bool) -> Self {
        let mut twiddle: Vec<FFTComplex> = Vec::with_capacity(size / 4);
        let factor = 2.0 * consts::PI / ((8 * size) as f32);
        let scale = if scaledown { (1.0 / (size as f32)).sqrt() } else { 1.0 };
        for k in 0..size/4 {
            twiddle.push(FFTComplex::exp(factor * ((8 * k + 1) as f32)).scale(scale));
        }
        let fft = FFTBuilder::new_fft(size/4, false);
        let mut z: Vec<FFTComplex> = Vec::with_capacity(size / 2);
        z.resize(size / 2, FFTC_ZERO);
        IMDCT { twiddle, fft, size, z }
    }
    /// Calculates IMDCT.
    pub fn imdct(&mut self, src: &[f32], dst: &mut [f32]) {
        let size2 = self.size / 2;
        let size4 = self.size / 4;
        let size8 = self.size / 8;
        for k in 0..size4 {
            let c = FFTComplex { re: src[size2 - 2 * k - 1], im: src[        2 * k] };
            self.z[k] = c * self.twiddle[k];
        }
        self.fft.do_ifft_inplace(&mut self.z);
        for k in 0..size4 {
            self.z[k] *= self.twiddle[k];
        }
        for n in 0..size8 {
            dst[            2 * n]     = -self.z[size8 + n]    .im;
            dst[            2 * n + 1] =  self.z[size8 - n - 1].re;
            dst[    size4 + 2 * n]     = -self.z[        n]    .re;
            dst[    size4 + 2 * n + 1] =  self.z[size4 - n - 1].im;
            dst[    size2 + 2 * n]     = -self.z[size8 + n]    .re;
            dst[    size2 + 2 * n + 1] =  self.z[size8 - n - 1].im;
            dst[3 * size4 + 2 * n]     =  self.z[        n]    .im;
            dst[3 * size4 + 2 * n + 1] = -self.z[size4 - n - 1].re;
        }
    }
    /// Calculates only non-mirrored part of IMDCT.
    pub fn imdct_half(&mut self, src: &[f32], dst: &mut [f32]) {
        let size2 = self.size / 2;
        let size4 = self.size / 4;
        let size8 = self.size / 8;
        for k in 0..size4 {
            let c = FFTComplex { re: src[size2 - 2 * k - 1], im: src[        2 * k] };
            self.z[k] = c * self.twiddle[k];
        }
        self.fft.do_ifft_inplace(&mut self.z);
        for k in 0..size4 {
            self.z[k] *= self.twiddle[k];
        }
        for n in 0..size8 {
            dst[        2 * n]     = -self.z[        n]    .re;
            dst[        2 * n + 1] =  self.z[size4 - n - 1].im;
            dst[size4 + 2 * n]     = -self.z[size8 + n]    .re;
            dst[size4 + 2 * n + 1] =  self.z[size8 - n - 1].im;
        }
    }
}
