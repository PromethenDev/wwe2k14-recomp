use crate::formats::{NAChannelMap, NAChannelType};
use crate::frame::{NAAudioInfo, NABufferType};
use super::*;

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
        let cur_gcd = Self::gcd(a, b);
        Self {
            num:    a / cur_gcd,
            den:    b / cur_gcd,
        }
    }
    fn gcd(mut a: usize, mut b: usize) -> usize {
        while a != 0 && b != 0 {
            if a >= b {
                a -= b;
            } else {
                b -= a;
            }
        }
        a.max(b)
    }
}

fn bessel_i0(mut x: f64) -> f64 {
    let mut i0 = 1.0;
    let mut ival = 1.0;
    let mut n = 1.0;
    x = x * x * 0.5;
    while ival > 1e-10 {
        ival *= x;
        ival /= n * n;
        n += 1.0;
        i0 += ival;
    }
    i0
}

trait Sinc {
    fn sinc(self) -> Self;
}

impl Sinc for f32 {
    fn sinc(self) -> Self { self.sin() / self }
}

const BESSEL_BETA: f64 = 8.0;
fn gen_sinc_coeffs(order: usize, num: usize, den: usize, norm: f32) -> Vec<f32> {
    let mut coeffs = vec![0.0; order * 2];

    let sinc_scale = std::f32::consts::PI * norm / (den as f32);
    let win_scale = 1.0 / ((order * den) as f32);
    for (i, coef) in coeffs.iter_mut().enumerate() {
        let pos = (((i as i32) - (order as i32)) * (den as i32) + (num as i32)) as f32;
        if pos == 0.0 {
            *coef = norm;
            continue;
        }
        let wval = f64::from((pos * win_scale).clamp(-1.0, 1.0));
        let win = bessel_i0(BESSEL_BETA - (1.0 - wval * wval).sqrt()) as f32;
        *coef = norm * (pos * sinc_scale).sinc() * win;
    }

    // norm
    let sum = coeffs.iter().fold(0.0f32, |acc, &x| acc + x) * norm;
    for el in coeffs.iter_mut() {
        *el /= sum;
    }

    coeffs
}

/// Context for continuous audio resampling and format conversion.
pub struct NAResample {
    coeffs:     Vec<Vec<f32>>,
    ratio:      Fraction,
    order:      usize,

    pos:        FracPos,
    hist_i:     Vec<Vec<i32>>,
    hist_f:     Vec<Vec<f32>>,
    deficit:    usize,

    dst_info:   NAAudioInfo,
    dst_chmap:  NAChannelMap,
    src_rate:   u32,
}

impl NAResample {
    pub fn new(src_rate: u32, dst_info: &NAAudioInfo, dst_chmap: &NAChannelMap, order: usize) -> Self {
        let ratio = Fraction::new(src_rate as usize, dst_info.sample_rate as usize);
        let nchannels = usize::from(dst_info.channels);
        let mut coeffs = Vec::with_capacity(ratio.den);
        let norm = if ratio.num < ratio.den { 1.0 } else { (ratio.den as f32) / (ratio.num as f32) };
        for i in 0..ratio.den {
            coeffs.push(gen_sinc_coeffs(order, i, ratio.den, norm));
        }

        let mut hist_i = Vec::with_capacity(nchannels);
        let mut hist_f = Vec::with_capacity(nchannels);
        if !dst_info.format.is_float() {
            for _ in 0..(order * 2) {
                hist_i.push(vec![0; nchannels]);
            }
        } else {
            for _ in 0..(order * 2) {
                hist_f.push(vec![0.0; nchannels]);
            }
        }

        Self {
            order, coeffs, ratio,
            pos:        FracPos::new(),
            hist_i, hist_f,
            deficit:    0,

            dst_info:   *dst_info,
            dst_chmap:  dst_chmap.clone(),
            src_rate,
        }
    }
    fn reinit(&mut self, src_rate: u32) {
        self.src_rate = src_rate;
        self.ratio = Fraction::new(src_rate as usize, self.dst_info.sample_rate as usize);
        self.coeffs.clear();
        let norm = if self.ratio.num < self.ratio.den { 1.0 } else { (self.ratio.den as f32) / (self.ratio.num as f32) };
        for i in 0..self.ratio.den {
            self.coeffs.push(gen_sinc_coeffs(self.order, i, self.ratio.den, norm));
        }
        self.pos = FracPos::new();
    }
    fn estimate_output(&self, nsamples: usize) -> usize {
        ((nsamples + 1) * self.ratio.den - self.pos.frac) / self.ratio.num + 16
    }
    fn add_samples_i32(&mut self, samples: &[i32]) -> bool {
        if self.deficit == 0 {
            return false;
        }
        self.deficit -= 1;
        self.hist_i[self.deficit].copy_from_slice(samples);
        true
    }
    fn get_samples_i32(&mut self, samples: &mut [i32]) -> bool {
        if self.deficit != 0 {
            return false;
        }
        let ret = self.pos.ipos == 0;
        if self.pos.ipos == 0 {
            for (i, dst) in samples.iter_mut().enumerate() {
                let mut sum = 0.0;
                for (hist, &coef) in self.hist_i.iter().zip(self.coeffs[self.pos.frac].iter()) {
                    sum += (hist[i] as f32) * coef;
                }
                *dst = sum as i32;
            }
            self.pos.add(self.ratio);
        }
        if self.pos.ipos > 0 {
            self.deficit = self.pos.ipos.min(self.hist_i.len());
            for i in (0..(self.hist_i.len() - self.deficit)).rev() {
                self.hist_i.swap(i, i + self.deficit)
            }
            self.pos.ipos -= self.deficit;
        }
        ret
    }
    fn add_samples_f32(&mut self, samples: &[f32]) -> bool {
        if self.deficit == 0 {
            return false;
        }
        self.deficit -= 1;
        self.hist_f[self.deficit].copy_from_slice(samples);
        true
    }
    fn get_samples_f32(&mut self, samples: &mut [f32]) -> bool {
        if self.deficit != 0 {
            return false;
        }
        let ret = self.pos.ipos == 0;
        if self.pos.ipos == 0 {
            for (i, dst) in samples.iter_mut().enumerate() {
                let mut sum = 0.0;
                for (hist, &coef) in self.hist_f.iter().zip(self.coeffs[self.pos.frac].iter()) {
                    sum += hist[i]* coef;
                }
                *dst = sum;
            }
            self.pos.add(self.ratio);
        }
        if self.pos.ipos > 0 {
            self.deficit = self.pos.ipos.min(self.hist_f.len());
            for i in (0..(self.hist_f.len() - self.deficit)).rev() {
                self.hist_f.swap(i, i + self.deficit)
            }
            self.pos.ipos -= self.deficit;
        }
        ret
    }
    pub fn convert_audio_frame(&mut self, src: &NABufferType) -> Result<NABufferType, SoundConvertError> {
        let nsamples = src.get_audio_length();
        if nsamples == 0 {
            return Err(SoundConvertError::InvalidInput);
        }
        let src_chmap = src.get_chmap().unwrap();
        let src_info  = src.get_audio_info().unwrap();
        if (src_chmap.num_channels() == 0) || (self.dst_chmap.num_channels() == 0) {
            return Err(SoundConvertError::InvalidInput);
        }

        let needs_remix = src_chmap.num_channels() != self.dst_chmap.num_channels();
        let no_channel_needs = !needs_remix && channel_maps_equal(src_chmap, &self.dst_chmap);
        let needs_reorder = !needs_remix && !no_channel_needs && channel_maps_reordered(src_chmap, &self.dst_chmap);

        if src_info.sample_rate != self.src_rate {
            self.reinit(src_info.sample_rate);
        }
        let needs_resample = src_info.sample_rate != self.dst_info.sample_rate;

        let channel_op = if no_channel_needs {
                ChannelOp::Passthrough
            } else if needs_reorder {
                let reorder_mat = calculate_reorder_matrix(src_chmap, &self.dst_chmap);
                ChannelOp::Reorder(reorder_mat)
            } else if src_chmap.num_channels() > 1 {
                let remix_mat = calculate_remix_matrix(src_chmap, &self.dst_chmap);
                ChannelOp::Remix(remix_mat)
            } else {
                let mut dup_mat: Vec<bool> = Vec::with_capacity(self.dst_chmap.num_channels());
                for i in 0..self.dst_chmap.num_channels() {
                    let ch =  self.dst_chmap.get_channel(i);
                    if ch.is_left() || ch.is_right() || ch == NAChannelType::C {
                        dup_mat.push(true);
                    } else {
                        dup_mat.push(false);
                    }
                }
                ChannelOp::DupMono(dup_mat)
            };

        let src_fmt = src_info.get_format();
        let dst_fmt = self.dst_info.get_format();
        let mut no_conversion = src_fmt == dst_fmt;

        // packed PCM needs to be processed
        if no_conversion && matches!(src, NABufferType::AudioPacked(_)) && !src_fmt.is_packed() && ((src_fmt.bits % 8) == 0) {
            no_conversion = false;
        }

        if no_conversion && no_channel_needs && !needs_resample {
            return Ok(src.clone());
        }

        let dst_nsamples = if !needs_resample {
                nsamples
            } else {
                self.estimate_output(nsamples)
            };
        let ret = alloc_audio_buffer(self.dst_info, dst_nsamples, self.dst_chmap.clone());
        if ret.is_err() {
            return Err(SoundConvertError::AllocError);
        }
        let mut dst_buf = ret.unwrap();

        let sstep = src.get_audio_step().max(1);
        let dstep = dst_buf.get_audio_step().max(1);
        let sr: Box<dyn SampleReader> = match src {
                NABufferType::AudioU8(ref ab) => {
                    let stride = ab.get_stride();
                    let data = ab.get_data();
                    if !src_fmt.signed {
                        Box::new(GenericSampleReader { data, stride })
                    } else {
                        Box::new(S8SampleReader { data, stride })
                    }
                },
                NABufferType::AudioI16(ref ab) => {
                    let data = ab.get_data();
                    let stride = ab.get_stride();
                    Box::new(GenericSampleReader { data, stride })
                },
                NABufferType::AudioI32(ref ab) => {
                    let data = ab.get_data();
                    let stride = ab.get_stride();
                    Box::new(GenericSampleReader { data, stride })
                },
                NABufferType::AudioF32(ref ab) => {
                    let data = ab.get_data();
                    let stride = ab.get_stride();
                    Box::new(GenericSampleReader { data, stride })
                },
                NABufferType::AudioPacked(ref ab) => {
                    let data = ab.get_data();
                    Box::new(PackedSampleReader::new(data, src_fmt))
                },
                _ => unimplemented!(),
            };
        let mut sw: Box<dyn SampleWriter> = match dst_buf {
                NABufferType::AudioU8(ref mut ab) => {
                    let stride = ab.get_stride();
                    let data = ab.get_data_mut().unwrap();
                    Box::new(GenericSampleWriter { data, stride })
                },
                NABufferType::AudioI16(ref mut ab) => {
                    let stride = ab.get_stride();
                    let data = ab.get_data_mut().unwrap();
                    Box::new(GenericSampleWriter { data, stride })
                },
                NABufferType::AudioI32(ref mut ab) => {
                    let stride = ab.get_stride();
                    let data = ab.get_data_mut().unwrap();
                    Box::new(GenericSampleWriter { data, stride })
                },
                NABufferType::AudioF32(ref mut ab) => {
                    let stride = ab.get_stride();
                    let data = ab.get_data_mut().unwrap();
                    Box::new(GenericSampleWriter { data, stride })
                },
                NABufferType::AudioPacked(ref mut ab) => {
                    let data = ab.get_data_mut().unwrap();
                    Box::new(PackedSampleWriter::new(data, dst_fmt))
                },
                _ => unimplemented!(),
            };

        let into_float = dst_fmt.float;
        let mut new_len = 0;
        if !into_float {
            let mut svec = vec![0; src_chmap.num_channels()];
            let mut dvec = vec![0; self.dst_chmap.num_channels()];
            let mut tvec = vec![0; self.dst_chmap.num_channels()];
            let mut spos = 0;
            let mut dpos = 0;
            for _ in 0..nsamples {
                sr.get_samples_i32(spos, &mut svec);
                if !channel_op.is_remix() {
                    apply_channel_op(&channel_op, &svec, &mut dvec);
                } else {
                    remix_i32(&channel_op, &svec, &mut dvec);
                }
                if !needs_resample {
                    sw.store_samples_i32(dpos, &dvec);
                    dpos += dstep;
                } else {
                    while self.get_samples_i32(&mut tvec) {
                        sw.store_samples_i32(dpos, &tvec);
                        dpos += dstep;
                    }
                    self.add_samples_i32(&dvec);
                }
                spos += sstep;
            }
            if needs_resample {
                while self.get_samples_i32(&mut tvec) {
                    sw.store_samples_i32(dpos, &tvec);
                    dpos += dstep;
                }
                new_len = dpos / dstep;
            }
        } else {
            let mut svec = vec![0.0; src_chmap.num_channels()];
            let mut dvec = vec![0.0; self.dst_chmap.num_channels()];
            let mut tvec = vec![0.0; self.dst_chmap.num_channels()];
            let mut spos = 0;
            let mut dpos = 0;
            for _ in 0..nsamples {
                sr.get_samples_f32(spos, &mut svec);
                if !channel_op.is_remix() {
                    apply_channel_op(&channel_op, &svec, &mut dvec);
                } else {
                    remix_f32(&channel_op, &svec, &mut dvec);
                }
                if !needs_resample {
                    sw.store_samples_f32(dpos, &dvec);
                    dpos += dstep;
                } else {
                    while self.get_samples_f32(&mut tvec) {
                        sw.store_samples_f32(dpos, &tvec);
                        dpos += dstep;
                    }
                    self.add_samples_f32(&dvec);
                }
                spos += sstep;
            }
            if needs_resample {
                while self.get_samples_f32(&mut tvec) {
                    sw.store_samples_f32(dpos, &tvec);
                    dpos += dstep;
                }
                new_len = dpos / dstep;
            }
        }
        drop(sw);

        if new_len != 0 {
            dst_buf.truncate_audio(new_len);
        }

        Ok(dst_buf)
    }
}
