//! Sound format conversion.
//!
//! This module implements the functionality for conversion between different sound formats: packed or planar audio, 8-/16-/24-/32-bit, integer or floating point, different number of channels.
//! Eventually this might support resampling as well.
pub use crate::formats::{NASoniton,NAChannelMap};
pub use crate::frame::{NAAudioBuffer,NAAudioInfo,NABufferType};
use crate::formats::NAChannelType;
use crate::frame::alloc_audio_buffer;
use crate::io::byteio::*;
use crate::io::bitreader::*;
use std::f32::consts::SQRT_2;

mod resample;
pub use resample::NAResample;

/// A list specifying general sound conversion errors.
#[derive(Clone,Copy,Debug,PartialEq)]
pub enum SoundConvertError {
    /// Invalid input arguments.
    InvalidInput,
    /// Allocation failed.
    AllocError,
    /// Requested feature is not supported.
    Unsupported,
}

enum ChannelOp {
    Passthrough,
    Reorder(Vec<usize>),
    Remix(Vec<f32>),
    DupMono(Vec<bool>),
}

impl ChannelOp {
    fn is_remix(&self) -> bool {
        matches! (*self, ChannelOp::Remix(_) | ChannelOp::DupMono(_))
    }
}

fn apply_channel_op<T:Copy>(ch_op: &ChannelOp, src: &[T], dst: &mut [T]) {
    match *ch_op {
        ChannelOp::Passthrough => {
            dst.copy_from_slice(src);
        },
        ChannelOp::Reorder(ref reorder) => {
            for (out, idx) in dst.iter_mut().zip(reorder.iter()) {
                *out = src[*idx];
            }
        },
        _ => {},
    };
}

fn remix_i32(ch_op: &ChannelOp, src: &[i32], dst: &mut [i32]) {
    if let ChannelOp::Remix(ref remix_mat) = ch_op {
        let sch = src.len();
        for (out, coeffs) in dst.iter_mut().zip(remix_mat.chunks(sch)) {
            let mut sum = 0.0;
            for (inval, coef) in src.iter().zip(coeffs.iter()) {
                sum += (*inval as f32) * *coef;
            }
            *out = sum as i32;
        }
    }
    if let ChannelOp::DupMono(ref dup_mat) = ch_op {
        let src = src[0];
        for (out, copy) in dst.iter_mut().zip(dup_mat.iter()) {
            *out = if *copy { src } else { 0 };
        }
    }
}

fn remix_f32(ch_op: &ChannelOp, src: &[f32], dst: &mut [f32]) {
    if let ChannelOp::Remix(ref remix_mat) = ch_op {
        let sch = src.len();
        for (out, coeffs) in dst.iter_mut().zip(remix_mat.chunks(sch)) {
            let mut sum = 0.0;
            for (inval, coef) in src.iter().zip(coeffs.iter()) {
                sum += *inval * *coef;
            }
            *out = sum;
        }
    }
    if let ChannelOp::DupMono(ref dup_mat) = ch_op {
        let src = src[0];
        for (out, copy) in dst.iter_mut().zip(dup_mat.iter()) {
            *out = if *copy { src } else { 0.0 };
        }
    }
}

trait FromFmt<T:Copy> {
    fn cvt_from(val: T) -> Self;
}

impl FromFmt<u8> for u8 {
    fn cvt_from(val: u8) -> u8 { val }
}
impl FromFmt<u8> for i16 {
    fn cvt_from(val: u8) -> i16 { u16::from(val ^ 0x80).wrapping_mul(0x101) as i16}
}
impl FromFmt<u8> for i32 {
    fn cvt_from(val: u8) -> i32 { u32::from(val ^ 0x80).wrapping_mul(0x01010101) as i32 }
}
impl FromFmt<u8> for f32 {
    fn cvt_from(val: u8) -> f32 { (f32::from(val) - 128.0) / 128.0 }
}

impl FromFmt<i16> for u8 {
    fn cvt_from(val: i16) -> u8 { ((val >> 8) + 128).clamp(0, 255) as u8 }
}
impl FromFmt<i16> for i16 {
    fn cvt_from(val: i16) -> i16 { val }
}
impl FromFmt<i16> for i32 {
    fn cvt_from(val: i16) -> i32 { (i32::from(val) & 0xFFFF) | (i32::from(val) << 16) }
}
impl FromFmt<i16> for f32 {
    fn cvt_from(val: i16) -> f32 { f32::from(val) / 32768.0 }
}

impl FromFmt<i32> for u8 {
    fn cvt_from(val: i32) -> u8 { ((val >> 24) + 128).clamp(0, 255) as u8 }
}
impl FromFmt<i32> for i16 {
    fn cvt_from(val: i32) -> i16 { (val >> 16) as i16 }
}
impl FromFmt<i32> for i32 {
    fn cvt_from(val: i32) -> i32 { val }
}
impl FromFmt<i32> for f32 {
    fn cvt_from(val: i32) -> f32 { (val as f32) / 31.0f32.exp2() }
}

impl FromFmt<f32> for u8 {
    fn cvt_from(val: f32) -> u8 { ((val * 128.0) + 128.0).clamp(0.0, 255.0) as u8 }
}
impl FromFmt<f32> for i16 {
    fn cvt_from(val: f32) -> i16 { (val * 32768.0).clamp(-32768.0, 32767.0) as i16 }
}
impl FromFmt<f32> for i32 {
    fn cvt_from(val: f32) -> i32 {
        if val >= 1.0 {
            i32::MAX
        } else if val <= -1.0 {
            i32::MIN
        } else {
            let scale = (1u32 << 31) as f32;
            (val * scale) as i32
        }
    }
}
impl FromFmt<f32> for f32 {
    fn cvt_from(val: f32) -> f32 { val }
}

trait IntoFmt<T:Copy> {
    fn cvt_into(self) -> T;
}

impl<T:Copy, U:Copy> IntoFmt<U> for T where U: FromFmt<T> {
    fn cvt_into(self) -> U { U::cvt_from(self) }
}


trait SampleReader {
    fn get_samples_i32(&self, pos: usize, dst: &mut [i32]);
    fn get_samples_f32(&self, pos: usize, dst: &mut [f32]);
}

struct GenericSampleReader<'a, T:Copy> {
    data:   &'a [T],
    stride: usize,
}

impl<'a, T:Copy+IntoFmt<i32>+IntoFmt<f32>> SampleReader for GenericSampleReader<'a, T> {
    fn get_samples_i32(&self, pos: usize, dst: &mut [i32]) {
        let mut off = pos;
        for el in dst.iter_mut() {
            *el = self.data[off].cvt_into();
            off += self.stride;
        }
    }
    fn get_samples_f32(&self, pos: usize, dst: &mut [f32]) {
        let mut off = pos;
        for el in dst.iter_mut() {
            *el = self.data[off].cvt_into();
            off += self.stride;
        }
    }
}

struct S8SampleReader<'a> {
    data:   &'a [u8],
    stride: usize,
}

impl<'a> SampleReader for S8SampleReader<'a> {
    fn get_samples_i32(&self, pos: usize, dst: &mut [i32]) {
        let mut off = pos;
        for el in dst.iter_mut() {
            *el = (self.data[off] ^ 0x80).cvt_into();
            off += self.stride;
        }
    }
    fn get_samples_f32(&self, pos: usize, dst: &mut [f32]) {
        let mut off = pos;
        for el in dst.iter_mut() {
            *el = (self.data[off] ^ 0x80).cvt_into();
            off += self.stride;
        }
    }
}

struct PackedSampleReader<'a> {
    data:   &'a [u8],
    fmt:    NASoniton,
    bpp:    usize,
}

impl<'a> PackedSampleReader<'a> {
    fn new(data: &'a [u8], fmt: NASoniton) -> Self {
        let bpp = (fmt.bits >> 3) as usize;
        Self { data, fmt, bpp }
    }
    fn get_samples<T:Copy>(&self, pos: usize, dst: &mut [T]) where u8: IntoFmt<T>, i16: IntoFmt<T>, i32: IntoFmt<T>, f32: IntoFmt<T> {
        if (self.fmt.bits & 7) != 0 {
            let mode = if self.fmt.be { BitReaderMode::BE } else { BitReaderMode::LE };
            let mut br = BitReader::new(self.data, mode);
            let offset = pos * (self.fmt.bits as usize) * dst.len();

            if br.skip(offset as u32).is_ok() {
                for el in dst.iter_mut() {
                    if self.fmt.bits < 16 {
                        *el = ((br.read(self.fmt.bits).unwrap_or(0) as i16) << (16 - self.fmt.bits)).cvt_into();
                    } else {
                        *el = ((br.read(self.fmt.bits).unwrap_or(0) as i32) << (32 - self.fmt.bits)).cvt_into();
                    }
                }
            } else {
                for el in dst.iter_mut() {
                    *el = 0i16.cvt_into();
                }
            }

            return;
        }

        let mut offset = pos * self.bpp * dst.len();

        for el in dst.iter_mut() {
            let src = &self.data[offset..];
            *el = if !self.fmt.float {
                    match (self.bpp, self.fmt.be) {
                        (1, _)     => if !self.fmt.signed { src[0].cvt_into() } else { (src[0] ^ 0x80).cvt_into() },
                        (2, true)  => (read_u16be(src).unwrap() as i16).cvt_into(),
                        (2, false) => (read_u16le(src).unwrap() as i16).cvt_into(),
                        (3, true)  => ((read_u24be(src).unwrap() << 8) as i32).cvt_into(),
                        (3, false) => ((read_u24be(src).unwrap() << 8) as i32).cvt_into(),
                        (4, true)  => (read_u32be(src).unwrap() as i32).cvt_into(),
                        (4, false) => (read_u32be(src).unwrap() as i32).cvt_into(),
                        _ => unreachable!(),
                    }
                } else {
                    match (self.bpp, self.fmt.be) {
                        (4, true)  => read_f32be(src).unwrap().cvt_into(),
                        (4, false) => read_f32le(src).unwrap().cvt_into(),
                        (8, true)  => (read_f64be(src).unwrap() as f32).cvt_into(),
                        (8, false) => (read_f64le(src).unwrap() as f32).cvt_into(),
                        (_, _) => unreachable!(),
                    }
                };
            offset += self.bpp;
        }
    }
}

impl SampleReader for PackedSampleReader<'_> {
    fn get_samples_i32(&self, pos: usize, dst: &mut [i32]) {
        self.get_samples(pos, dst);
    }
    fn get_samples_f32(&self, pos: usize, dst: &mut [f32]) {
        self.get_samples(pos, dst);
    }
}

trait SampleWriter {
    fn store_samples_i32(&mut self, pos: usize, src: &[i32]);
    fn store_samples_f32(&mut self, pos: usize, src: &[f32]);
}

struct GenericSampleWriter<'a, T:Copy> {
    data:   &'a mut [T],
    stride: usize,
}

impl<'a, T:Copy+FromFmt<i32>+FromFmt<f32>> SampleWriter for GenericSampleWriter<'a, T> {
    fn store_samples_i32(&mut self, pos: usize, src: &[i32]) {
        let mut off = pos;
        for el in src.iter() {
            self.data[off] = (*el).cvt_into();
            off += self.stride;
        }
    }
    fn store_samples_f32(&mut self, pos: usize, src: &[f32]) {
        let mut off = pos;
        for el in src.iter() {
            self.data[off] = (*el).cvt_into();
            off += self.stride;
        }
    }
}

struct PackedSampleWriter<'a> {
    data:   &'a mut [u8],
    fmt:    NASoniton,
    bpp:    usize,
}

impl<'a> PackedSampleWriter<'a> {
    fn new(data: &'a mut [u8], fmt: NASoniton) -> Self {
        if (fmt.bits & 7) != 0 { unimplemented!(); }
        let bpp = (fmt.bits >> 3) as usize;
        Self { data, fmt, bpp }
    }

    fn store_samples<T:Copy>(&mut self, pos: usize, src: &[T]) where u8: FromFmt<T>, i16: FromFmt<T>, i32: FromFmt<T>, f32: FromFmt<T> {
        let mut offset = pos * self.bpp * src.len();
        for el in src.iter() {
            let dst = &mut self.data[offset..];
            if !self.fmt.float {
                match (self.bpp, self.fmt.be) {
                    (1, _) => {
                        dst[0] = u8::cvt_from(*el);
                        if self.fmt.signed {
                            dst[0] ^= 0x80;
                        }
                    },
                    (2, true)  => write_u16be(dst, i16::cvt_from(*el) as u16).unwrap(),
                    (2, false) => write_u16le(dst, i16::cvt_from(*el) as u16).unwrap(),
                    (3, true)  => write_u24be(dst, (i32::cvt_from(*el) >> 8) as u32).unwrap(),
                    (3, false) => write_u24le(dst, (i32::cvt_from(*el) >> 8) as u32).unwrap(),
                    (4, true)  => write_u32be(dst, i32::cvt_from(*el) as u32).unwrap(),
                    (4, false) => write_u32le(dst, i32::cvt_from(*el) as u32).unwrap(),
                    _ => unreachable!(),
                };
            } else {
                match (self.bpp, self.fmt.be) {
                    (4, true)  => write_f32be(dst, f32::cvt_from(*el)).unwrap(),
                    (4, false) => write_f32le(dst, f32::cvt_from(*el)).unwrap(),
                    (8, true)  => write_f64be(dst, f64::from(f32::cvt_from(*el))).unwrap(),
                    (8, false) => write_f64le(dst, f64::from(f32::cvt_from(*el))).unwrap(),
                    (_, _) => unreachable!(),
                };
            }
            offset += self.bpp;
        }
    }
}

impl SampleWriter for PackedSampleWriter<'_> {
    fn store_samples_i32(&mut self, pos: usize, src: &[i32]) {
        self.store_samples(pos, src);
    }
    fn store_samples_f32(&mut self, pos: usize, src: &[f32]) {
        self.store_samples(pos, src);
    }
}

/// Converts input audio buffer into desired format and returns a newly allocated buffer.
pub fn convert_audio_frame(src: &NABufferType, dst_info: &NAAudioInfo, dst_chmap: &NAChannelMap) ->
Result<NABufferType, SoundConvertError> {
    let nsamples = src.get_audio_length();
    if nsamples == 0 {
        return Err(SoundConvertError::InvalidInput);
    }
    let src_chmap = src.get_chmap().unwrap();
    let src_info  = src.get_audio_info().unwrap();
    if src_info.sample_rate != dst_info.sample_rate {
        return Err(SoundConvertError::Unsupported);
    }
    if (src_chmap.num_channels() == 0) || (dst_chmap.num_channels() == 0) {
        return Err(SoundConvertError::InvalidInput);
    }

    let needs_remix = src_chmap.num_channels() != dst_chmap.num_channels();
    let no_channel_needs = !needs_remix && channel_maps_equal(src_chmap, dst_chmap);
    let needs_reorder = !needs_remix && !no_channel_needs && channel_maps_reordered(src_chmap, dst_chmap);

    let channel_op = if no_channel_needs {
            ChannelOp::Passthrough
        } else if needs_reorder {
            let reorder_mat = calculate_reorder_matrix(src_chmap, dst_chmap);
            ChannelOp::Reorder(reorder_mat)
        } else if src_chmap.num_channels() > 1 {
            let remix_mat = calculate_remix_matrix(src_chmap, dst_chmap);
            ChannelOp::Remix(remix_mat)
        } else {
            let mut dup_mat: Vec<bool> = Vec::with_capacity(dst_chmap.num_channels());
            for i in 0..dst_chmap.num_channels() {
                let ch =  dst_chmap.get_channel(i);
                if ch.is_left() || ch.is_right() || ch == NAChannelType::C {
                    dup_mat.push(true);
                } else {
                    dup_mat.push(false);
                }
            }
            ChannelOp::DupMono(dup_mat)
        };

    let src_fmt = src_info.get_format();
    let dst_fmt = dst_info.get_format();
    let no_conversion = src_fmt == dst_fmt;

    if no_conversion && no_channel_needs {
        return Ok(src.clone());
    }

    let ret = alloc_audio_buffer(*dst_info, nsamples, dst_chmap.clone());
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
    if !into_float {
        let mut svec = vec![0; src_chmap.num_channels()];
        let mut dvec = vec![0; dst_chmap.num_channels()];
        let mut spos = 0;
        let mut dpos = 0;
        for _ in 0..nsamples {
            sr.get_samples_i32(spos, &mut svec);
            if !channel_op.is_remix() {
                apply_channel_op(&channel_op, &svec, &mut dvec);
            } else {
                remix_i32(&channel_op, &svec, &mut dvec);
            }
            sw.store_samples_i32(dpos, &dvec);
            spos += sstep;
            dpos += dstep;
        }
    } else {
        let mut svec = vec![0.0; src_chmap.num_channels()];
        let mut dvec = vec![0.0; dst_chmap.num_channels()];
        let mut spos = 0;
        let mut dpos = 0;
        for _ in 0..nsamples {
            sr.get_samples_f32(spos, &mut svec);
            if !channel_op.is_remix() {
                apply_channel_op(&channel_op, &svec, &mut dvec);
            } else {
                remix_f32(&channel_op, &svec, &mut dvec);
            }
            sw.store_samples_f32(dpos, &dvec);
            spos += sstep;
            dpos += dstep;
        }
    }
    drop(sw);

    Ok(dst_buf)
}

/// Checks whether two channel maps are identical.
pub fn channel_maps_equal(a: &NAChannelMap, b: &NAChannelMap) -> bool {
    if a.num_channels() != b.num_channels() { return false; }
    for i in 0..a.num_channels() {
        if a.get_channel(i) != b.get_channel(i) {
            return false;
        }
    }
    true
}

/// Checks whether two channel maps have identical channels (but maybe in different order).
pub fn channel_maps_reordered(a: &NAChannelMap, b: &NAChannelMap) -> bool {
    if a.num_channels() != b.num_channels() { return false; }
    let mut count_a = [0u8; 32];
    let mut count_b = [0u8; 32];
    for i in 0..a.num_channels() {
        count_a[a.get_channel(i) as usize] += 1;
        count_b[b.get_channel(i) as usize] += 1;
    }
    for (c0, c1) in count_a.iter().zip(count_b.iter()) {
        if *c0 != *c1 {
            return false;
        }
    }
    true
}

/// Calculates permutation matrix for reordering channels from source channel map into destination one.
pub fn calculate_reorder_matrix(src: &NAChannelMap, dst: &NAChannelMap) -> Vec<usize> {
    if src.num_channels() != dst.num_channels() { return Vec::new(); }
    let num_channels = src.num_channels();
    let mut reorder: Vec<usize> = Vec::with_capacity(num_channels);
    for i in 0..num_channels {
        let dst_ch = dst.get_channel(i);
        for j in 0..num_channels {
            if src.get_channel(j) == dst_ch {
                reorder.push(j);
                break;
            }
        }
    }
    if reorder.len() != num_channels { reorder.clear(); }
    reorder
}

fn is_stereo(chmap: &NAChannelMap) -> bool {
    (chmap.num_channels() == 2) &&
    (chmap.get_channel(0) == NAChannelType::L) &&
    (chmap.get_channel(1) == NAChannelType::R)
}

/// Calculates matrix of remixing coefficients for converting input channel layout into destination one.
pub fn calculate_remix_matrix(src: &NAChannelMap, dst: &NAChannelMap) -> Vec<f32> {
    // common case: stereo -> mono downmix
    if is_stereo(src) && dst.num_channels() == 1 &&
        (dst.get_channel(0) == NAChannelType::L || dst.get_channel(0) == NAChannelType::C) {
        return vec![0.5, 0.5];
    }
    // common case: mono -> stereo upmix
    if src.num_channels() == 1 && is_stereo(dst) &&
        (src.get_channel(0) == NAChannelType::L || src.get_channel(0) == NAChannelType::C) {
        return vec![1.0, 1.0];
    }
    // still rather common case: 5.x/6.x -> stereo downmix
    if src.num_channels() >= 5 && is_stereo(dst) {
        let src_nch = src.num_channels();
        let mut mat = vec![0.0f32; src_nch * 2];
        let (l_mat, r_mat) = mat.split_at_mut(src_nch);
        for ch in 0..src_nch {
            match src.get_channel(ch) {
                NAChannelType::L    => l_mat[ch] = 1.0,
                NAChannelType::R    => r_mat[ch] = 1.0,
                NAChannelType::C    => { l_mat[ch] = SQRT_2 / 2.0; r_mat[ch] = SQRT_2 / 2.0; },
                NAChannelType::Ls   => l_mat[ch] = SQRT_2 / 2.0,
                NAChannelType::Rs   => r_mat[ch] = SQRT_2 / 2.0,
                _ => {},
            };
        }
        return mat;
    }
    // rather specific case: multichannel -> C
    // use L+R -> C in this case instead as some files have nothing in centre channel
    if src.num_channels() > 2 && dst.num_channels() == 1 && dst.get_channel(0) == NAChannelType::C {
        if let (Some(lidx), Some(ridx)) = (src.find_channel_id(NAChannelType::L), src.find_channel_id(NAChannelType::R)) {
            let mut remix = vec![0.0; src.num_channels()];
            remix[lidx] = SQRT_2 / 2.0;
            remix[ridx] = SQRT_2 / 2.0;
            return remix;
        }
    }
    // generic possible case: target includes all channels from source
    let dst_ch = dst.num_channels();
    let mut remix = vec![0.0; src.num_channels() * dst_ch];
    let mut mapped = vec![false; dst_ch];

    for (&sch, mix_coeffs) in src.iter().zip(remix.chunks_exact_mut(dst_ch)) {
        for ((coef, &dch), mapped_ch) in mix_coeffs.iter_mut().zip(dst.iter())
                .zip(mapped.iter_mut()) {
            if dch == sch && !*mapped_ch {
                *coef = 1.0;
                *mapped_ch = true;
            }
        }
    }
    if !mapped.contains(&false) {
        return remix;
    }
    // try to fill gaps by mono <-> stereo conversions
    for (didx, (&dch, mapped_ch)) in dst.iter().zip(mapped.iter_mut()).enumerate() {
        if *mapped_ch {
            continue;
        }
        if let Some((lch, rch)) = dch.get_pair() {
            if let (Some(lidx), Some(ridx)) = (src.find_channel_id(lch), src.find_channel_id(rch)) {
                remix[lidx * dst_ch + didx] = SQRT_2 / 2.0;
                remix[ridx * dst_ch + didx] = SQRT_2 / 2.0;
                *mapped_ch = true;
            }
        }
    }
    for (&sch, mix_coef) in src.iter().zip(remix.chunks_exact_mut(dst_ch)) {
        if let Some((lch, rch)) = sch.get_pair() {
            if let (Some(lidx), Some(ridx)) = (dst.find_channel_id(lch), src.find_channel_id(rch)) {
                if !mapped[lidx] && !mapped[ridx] {
                    mix_coef[lidx] = 1.0;
                    mix_coef[ridx] = 1.0;
                    mapped[lidx] = true;
                    mapped[ridx] = true;
                }
            }
        }
    }
    if !mapped.contains(&true) {
        print!("failed to find any map from");
        for ch in src.iter() { print!(" {ch}"); }
        print!(" to");
        for ch in dst.iter() { print!(" {ch}"); }
        println!();
unimplemented!();
    }
    remix
}

#[cfg(test)]
mod test {
    use super::*;
    use std::str::FromStr;
    use crate::formats::*;

    #[test]
    fn test_matrices() {
        let chcfg51 = NAChannelMap::from_str("L,R,C,LFE,Ls,Rs").unwrap();
        let chcfg52 = NAChannelMap::from_str("C,L,R,Ls,Rs,LFE").unwrap();
        let stereo  = NAChannelMap::from_str("L,R").unwrap();
        let reorder = calculate_reorder_matrix(&chcfg51, &chcfg52);
        assert_eq!(reorder.as_slice(), [ 2, 0, 1, 4, 5, 3]);
        let remix   = calculate_remix_matrix(&chcfg51, &stereo);
        assert_eq!(remix.as_slice(), [ 1.0, 0.0, SQRT_2 / 2.0, 0.0, SQRT_2 / 2.0, 0.0,
                                       0.0, 1.0, SQRT_2 / 2.0, 0.0, 0.0, SQRT_2 / 2.0 ]);
    }
    #[test]
    fn test_conversion() {
        const CHANNEL_VALUES: [u8; 6] = [ 140, 90, 130, 128, 150, 70 ];
        let chcfg51 = NAChannelMap::from_str("L,R,C,LFE,Ls,Rs").unwrap();
        let stereo  = NAChannelMap::from_str("L,R").unwrap();
        let src_ainfo = NAAudioInfo {
                            sample_rate:    44100,
                            channels:       chcfg51.num_channels() as u8,
                            format:         SND_U8_FORMAT,
                            block_len:      512,
                        };
        let mut dst_ainfo = NAAudioInfo {
                            sample_rate:    44100,
                            channels:       stereo.num_channels() as u8,
                            format:         SND_S16P_FORMAT,
                            block_len:      512,
                        };
        let mut src_frm = alloc_audio_buffer(src_ainfo, 42, chcfg51.clone()).unwrap();
        if let NABufferType::AudioU8(ref mut abuf) = src_frm {
            let data = abuf.get_data_mut().unwrap();
            let mut idx = 0;
            for _ in 0..42 {
                for ch in 0..chcfg51.num_channels() {
                    data[idx] = CHANNEL_VALUES[ch];
                    idx += 1;
                }
            }
        } else {
            panic!("wrong buffer type");
        }

        let out_frm = convert_audio_frame(&src_frm, &dst_ainfo, &stereo).unwrap();
        if let NABufferType::AudioI16(ref abuf) = out_frm {
            let off0 = abuf.get_offset(0);
            let off1 = abuf.get_offset(1);
            let data = abuf.get_data();
            let l = data[off0];
            let r = data[off1];
            assert_eq!(l, 7445);
            assert_eq!(r, -19505);
        } else {
            panic!("wrong buffer type");
        }

        dst_ainfo.format = SND_F32P_FORMAT;
        let out_frm = convert_audio_frame(&src_frm, &dst_ainfo, &stereo).unwrap();
        if let NABufferType::AudioF32(ref abuf) = out_frm {
            let off0 = abuf.get_offset(0);
            let off1 = abuf.get_offset(1);
            let data = abuf.get_data();
            let l = data[off0];
            let r = data[off1];
            assert_eq!(l,  0.22633252);
            assert_eq!(r, -0.6062342);
        } else {
            panic!("wrong buffer type");
        }

        const PCM12: &[u8] = &[ 0x02, 0x50, 0x00, 0x07, 0x70, 0x00, 0x0D, 0x00 ];
        const PCM12_SAMPLES: usize = 5;
        let src_ainfo = NAAudioInfo {
                            sample_rate:    44100,
                            channels:       1,
                            format:         NASoniton::new(12, SONITON_FLAG_PACKED | SONITON_FLAG_SIGNED),
                            block_len:      3,
                        };
        let dst_ainfo = NAAudioInfo {
                            sample_rate:    44100,
                            channels:       1,
                            format:         SND_S16P_FORMAT,
                            block_len:      512,
                        };
        let mono = NAChannelMap::from_str("C").unwrap();
        let mut src_frm = alloc_audio_buffer(src_ainfo, PCM12_SAMPLES, mono.clone()).unwrap();
        if let NABufferType::AudioPacked(ref mut abuf) = src_frm {
            let data = abuf.get_data_mut().unwrap();
            data.copy_from_slice(PCM12);
        } else {
            panic!("wrong buffer type");
        }
        let out_frm = convert_audio_frame(&src_frm, &dst_ainfo, &mono).unwrap();
        if let NABufferType::AudioI16(ref abuf) = out_frm {
            let data = abuf.get_data();
            assert_eq!(data.len(), PCM12_SAMPLES);
            assert_eq!(data[0], 0x0020);
            assert_eq!(data[1], 0x0050);
            assert_eq!(data[2], 0x0070);
            assert_eq!(data[3], 0x0070);
            assert_eq!(data[4], 0x00D0);
        } else {
            panic!("wrong buffer type");
        }
    }
}
