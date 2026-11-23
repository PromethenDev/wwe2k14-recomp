//! Image conversion functionality.

//! # Examples
//!
//! Convert input image into YUV one and scale down two times.
//! ```no_run
//! use nihav_core::scale::*;
//! use nihav_core::formats::{RGB24_FORMAT, YUV420_FORMAT};
//! use nihav_core::frame::{alloc_video_buffer, NAVideoInfo};
//!
//! let mut in_pic = alloc_video_buffer(NAVideoInfo::new(640, 480, false, RGB24_FORMAT), 4).unwrap();
//! let mut out_pic = alloc_video_buffer(NAVideoInfo::new(320, 240, false, YUV420_FORMAT), 4).unwrap();
//! let in_fmt = get_scale_fmt_from_pic(&in_pic);
//! let out_fmt = get_scale_fmt_from_pic(&out_pic);
//! let mut scaler = NAScale::new(in_fmt, out_fmt).unwrap();
//! scaler.convert(&in_pic, &mut out_pic).unwrap();
//! ```
use crate::frame::*;

mod kernel;

mod colourcvt;
mod depth;
mod fill;
mod repack;
#[allow(clippy::module_inception)]
mod scale;

mod palette;

pub use crate::scale::palette::{palettise_frame, QuantisationMode, PaletteSearchMode};

/// Image format information used by the converter.
#[derive(Clone,Copy,PartialEq)]
pub struct ScaleInfo {
    /// Pixel format description.
    pub fmt:    NAPixelFormaton,
    /// Image width.
    pub width:  usize,
    /// Image height.
    pub height: usize,
}

impl std::fmt::Display for ScaleInfo {
    fn fmt(&self, f: &mut std::fmt::Formatter) -> std::fmt::Result {
        write!(f, "({}x{}, {})", self.width, self.height, self.fmt)
    }
}

/// A list specifying general image conversion errors.
#[derive(Debug,Clone,Copy,PartialEq)]
#[allow(dead_code)]
pub enum ScaleError {
    /// Input or output buffer contains no image data.
    NoFrame,
    /// Allocation failed.
    AllocError,
    /// Invalid argument.
    InvalidArgument,
    /// Feature is not implemented.
    NotImplemented,
    /// Internal implementation bug.
    Bug,
}

/// A specialised `Result` type for image conversion operations.
pub type ScaleResult<T> = Result<T, ScaleError>;

/*trait Kernel {
    fn init(&mut self, in_fmt: &ScaleInfo, dest_fmt: &ScaleInfo) -> ScaleResult<NABufferType>;
    fn process(&mut self, pic_in: &NABufferType, pic_out: &mut NABufferType);
}*/

struct KernelDesc {
    name:       &'static str,
    create:     fn () -> Box<dyn kernel::Kernel>,
}

impl KernelDesc {
    fn find(name: &str) -> ScaleResult<Box<dyn kernel::Kernel>> {
        for kern in KERNELS.iter() {
            if kern.name == name {
                return Ok((kern.create)());
            }
        }
        Err(ScaleError::InvalidArgument)
    }
}

const KERNELS: &[KernelDesc] = &[
    KernelDesc { name: "pack",          create: repack::create_pack },
    KernelDesc { name: "unpack",        create: repack::create_unpack },
    KernelDesc { name: "depal",         create: repack::create_depal },
    KernelDesc { name: "palette",       create: palette::create_palettise },
    KernelDesc { name: "scale",         create: scale::create_scale },
    KernelDesc { name: "shallow",       create: depth::create_shallow },
    KernelDesc { name: "fill",          create: fill::create_fill },
    KernelDesc { name: "rgb_to_yuv",    create: colourcvt::create_rgb2yuv },
    KernelDesc { name: "yuv_to_rgb",    create: colourcvt::create_yuv2rgb },
];

struct Stage {
    fmt_out:    ScaleInfo,
    tmp_pic:    NABufferType,
    next:       Option<Box<Stage>>,
    worker:     Box<dyn kernel::Kernel>,
}

/// Converts input picture information into format used by scaler.
pub fn get_scale_fmt_from_pic(pic: &NABufferType) -> ScaleInfo {
    let info = pic.get_video_info().unwrap();
    ScaleInfo { fmt: info.get_format(), width: info.get_width(), height: info.get_height() }
}

impl Stage {
    fn new(name: &str, in_fmt: &ScaleInfo, dest_fmt: &ScaleInfo, options: &[(String, String)]) -> ScaleResult<Self> {
        let mut worker = KernelDesc::find(name)?;
        let tmp_pic = worker.init(in_fmt, dest_fmt, options)?;
        let fmt_out = get_scale_fmt_from_pic(&tmp_pic);
        Ok(Self { fmt_out, tmp_pic, next: None, worker })
    }
    fn add(&mut self, new: Stage) {
        if let Some(ref mut next) = self.next {
            next.add(new);
        } else {
            self.next = Some(Box::new(new));
        }
    }
    fn process(&mut self, pic_in: &NABufferType, pic_out: &mut NABufferType) -> ScaleResult<()> {
        if let Some(ref mut nextstage) = self.next {
            self.worker.process(pic_in, &mut self.tmp_pic);
            nextstage.process(&self.tmp_pic, pic_out)?;
        } else {
            self.worker.process(pic_in, pic_out);
        }
        Ok(())
    }
    fn drop_last_tmp(&mut self) {
        if let Some(ref mut nextstage) = self.next {
            nextstage.drop_last_tmp();
        } else {
            self.tmp_pic = NABufferType::None;
        }
    }
}

/// Image format converter.
pub struct NAScale {
    fmt_in:         ScaleInfo,
    fmt_out:        ScaleInfo,
    just_convert:   bool,
    pipeline:       Option<Stage>,
}

fn check_format(in_fmt: NAVideoInfo, ref_fmt: &ScaleInfo, just_convert: bool) -> ScaleResult<()> {
    if in_fmt.get_format() != ref_fmt.fmt { return Err(ScaleError::InvalidArgument); }
    if !just_convert && (in_fmt.get_width() != ref_fmt.width || in_fmt.get_height() != ref_fmt.height) {
        return Err(ScaleError::InvalidArgument);
    }
    Ok(())
}

fn copy(pic_in: &NABufferType, pic_out: &mut NABufferType)
{
    if let (Some(ref sbuf), Some(ref mut dbuf)) = (pic_in.get_vbuf(), pic_out.get_vbuf()) {
        if sbuf.get_info().get_format().is_paletted() {
            let same = sbuf.get_stride(0) == dbuf.get_stride(0) && sbuf.get_offset(1) == dbuf.get_offset(1);
            if same {
                let src = sbuf.get_data();
                let dst = dbuf.get_data_mut().unwrap();
                dst.copy_from_slice(src);
            } else {
                let (_, h) = sbuf.get_dimensions(0);
                let soff = sbuf.get_offset(0);
                let spoff = sbuf.get_offset(1);
                let sstride = sbuf.get_stride(0);
                let src = sbuf.get_data();
                let doff = dbuf.get_offset(0);
                let dpoff = dbuf.get_offset(1);
                let dstride = dbuf.get_stride(0);
                let dst = dbuf.get_data_mut().unwrap();
                let copy_size = sstride.min(dstride);
                for (dline, sline) in dst[doff..].chunks_exact_mut(dstride).take(h).zip(src[soff..].chunks_exact(sstride)) {
                    dline[..copy_size].copy_from_slice(&sline[..copy_size]);
                }
                dst[dpoff..].copy_from_slice(&src[spoff..]);
            }
            return;
        }
        let mut same = true;
        let src_components = sbuf.get_info().get_format().get_num_comp();
        let dst_components = dbuf.get_info().get_format().get_num_comp();
        for i in 0..src_components.max(dst_components) {
            if sbuf.get_stride(i) != dbuf.get_stride(i) {
                same = false;
                break;
            }
            if sbuf.get_offset(i) != dbuf.get_offset(i) {
                same = false;
                break;
            }
        }
        if same {
            let sdata = sbuf.get_data();
            let ddata = dbuf.get_data_mut().unwrap();
            let copy_len = sdata.len().min(ddata.len());
            ddata[..copy_len].copy_from_slice(&sdata[..copy_len]);
        } else {
            let sdata = sbuf.get_data();
            for comp in 0..src_components.min(dst_components) {
                let (_, h) = sbuf.get_dimensions(comp);
                let src = &sdata[sbuf.get_offset(comp)..];
                let sstride = sbuf.get_stride(comp);
                let doff = dbuf.get_offset(comp);
                let dstride = dbuf.get_stride(comp);
                let ddata = dbuf.get_data_mut().unwrap();
                let dst = &mut ddata[doff..];
                let copy_size = sstride.min(dstride);
                if sstride == 0 && dstride == 0 {
                    continue;
                }
                for (dline, sline) in dst.chunks_exact_mut(dstride).take(h).zip(src.chunks_exact(sstride)) {
                    dline[..copy_size].copy_from_slice(&sline[..copy_size]);
                }
            }
        }
    } else if let (Some(ref sbuf), Some(ref mut dbuf)) = (pic_in.get_vbuf16(), pic_out.get_vbuf16()) {
        let mut same = true;
        let src_components = sbuf.get_info().get_format().get_num_comp();
        let dst_components = dbuf.get_info().get_format().get_num_comp();
        for i in 0..src_components.max(dst_components) {
            if sbuf.get_stride(i) != dbuf.get_stride(i) {
                same = false;
                break;
            }
            if sbuf.get_offset(i) != dbuf.get_offset(i) {
                same = false;
                break;
            }
        }
        if same {
            let sdata = sbuf.get_data();
            let ddata = dbuf.get_data_mut().unwrap();
            let copy_len = sdata.len().min(ddata.len());
            ddata[..copy_len].copy_from_slice(&sdata[..copy_len]);
        } else {
            let sdata = sbuf.get_data();
            for comp in 0..src_components.min(dst_components) {
                let (_, h) = sbuf.get_dimensions(comp);
                let src = &sdata[sbuf.get_offset(comp)..];
                let sstride = sbuf.get_stride(comp);
                let doff = dbuf.get_offset(comp);
                let dstride = dbuf.get_stride(comp);
                let ddata = dbuf.get_data_mut().unwrap();
                let dst = &mut ddata[doff..];
                let copy_size = sstride.min(dstride);
                for (dline, sline) in dst.chunks_exact_mut(dstride).take(h).zip(src.chunks_exact(sstride)) {
                    dline[..copy_size].copy_from_slice(&sline[..copy_size]);
                }
            }
        }
    } else {
        unimplemented!();
    }
}

macro_rules! add_stage {
    ($head:expr, $new:expr) => {
        if let Some(ref mut h) = $head {
            h.add($new);
        } else {
            $head = Some($new);
        }
    };
}
fn is_better_fmt(a: &ScaleInfo, b: &ScaleInfo) -> bool {
    if (a.width >= b.width) && (a.height >= b.height) {
        return true;
    }
    if a.fmt.get_max_depth() > b.fmt.get_max_depth() {
        return true;
    }
    if a.fmt.get_max_subsampling() < b.fmt.get_max_subsampling() {
        return true;
    }
    false
}
fn fmt_needs_scale(ifmt: &NAPixelFormaton, ofmt: &NAPixelFormaton) -> bool {
    for (ichr, ochr) in ifmt.comp_info.iter().zip(ofmt.comp_info.iter()) {
        if let (Some(ic), Some(oc)) = (ichr, ochr) {
            if ic.h_ss != oc.h_ss || ic.v_ss != oc.v_ss {
                return true;
            }
        }
    }
    false
}
fn build_pipeline(ifmt: &ScaleInfo, ofmt: &ScaleInfo, just_convert: bool, options: &[(String, String)]) -> ScaleResult<Option<Stage>> {
    let mut debug = false;
    for (name, value) in options.iter() {
        if name == "debug" && (value.is_empty() || value == "true") {
            debug = true;
            break;
        }
    }

    let inname  = ifmt.fmt.get_model().get_short_name();
    let outname = ofmt.fmt.get_model().get_short_name();

    if debug {
        println!("convert {} -> {}", ifmt, ofmt);
    }
    let needs_scale = if fmt_needs_scale(&ifmt.fmt, &ofmt.fmt) {
            true
        } else {
            !just_convert
        };
    let needs_unpack = !ifmt.fmt.is_unpacked();
    let needs_pack = !ofmt.fmt.is_unpacked();
    let needs_convert = inname != outname;
    let scale_before_cvt = is_better_fmt(ifmt, ofmt) && needs_convert
                           && (ofmt.fmt.get_max_subsampling() == 0);
    let needs_palettise = ofmt.fmt.palette;
//todo stages for model and gamma conversion

    let mut stages: Option<Stage> = None;
    let mut cur_fmt = *ifmt;

    if needs_unpack {
        if debug {
            println!("[adding unpack]");
        }
        let new_stage = if !cur_fmt.fmt.is_paletted() {
                Stage::new("unpack", &cur_fmt, ofmt, options)?
            } else {
                Stage::new("depal", &cur_fmt, ofmt, options)?
            };
        cur_fmt = new_stage.fmt_out;
        add_stage!(stages, new_stage);
    }
    if needs_scale && scale_before_cvt {
        if debug {
            println!("[adding scale]");
        }
        let new_stage = Stage::new("scale", &cur_fmt, ofmt, options)?;
        cur_fmt = new_stage.fmt_out;
        add_stage!(stages, new_stage);
    }
    if needs_convert {
        if debug {
            println!("[adding convert]");
        }
        let cvtname = format!("{}_to_{}", inname, outname);
        if debug {
            println!("[{}]", cvtname);
        }
        let new_stage = Stage::new(&cvtname, &cur_fmt, ofmt, options)?;
//todo if fails try converting via RGB or YUV
        cur_fmt = new_stage.fmt_out;
        add_stage!(stages, new_stage);
//todo alpha plane copy/add
    }
    if needs_scale && !scale_before_cvt {
        if debug {
            println!("[adding scale]");
        }
        let new_stage = Stage::new("scale", &cur_fmt, ofmt, options)?;
        cur_fmt = new_stage.fmt_out;
        add_stage!(stages, new_stage);
    }
    let is_in_high_bd = cur_fmt.fmt.get_max_depth() > 8;
    let is_out_high_bd = ofmt.fmt.get_max_depth() > 8;
    if is_in_high_bd && !is_out_high_bd {
        if debug {
            println!("[adding shallow]");
        }
        let new_stage = Stage::new("shallow", &cur_fmt, ofmt, options)?;
        cur_fmt = new_stage.fmt_out;
        add_stage!(stages, new_stage);
    }
    let icomponents = cur_fmt.fmt.components - if cur_fmt.fmt.alpha { 1 } else { 0 };
    let ocomponents = ofmt.fmt.components - if ofmt.fmt.alpha { 1 } else { 0 };
    if !needs_palettise && ((!cur_fmt.fmt.alpha && ofmt.fmt.alpha) || (icomponents < ocomponents)) {
        if debug {
            println!("[adding fill]");
        }
        let new_stage = Stage::new("fill", &cur_fmt, ofmt, options)?;
        cur_fmt = new_stage.fmt_out;
        add_stage!(stages, new_stage);
    }
    if needs_pack && !needs_palettise {
        if debug {
            println!("[adding pack]");
        }
        let new_stage = Stage::new("pack", &cur_fmt, ofmt, options)?;
        //cur_fmt = new_stage.fmt_out;
        add_stage!(stages, new_stage);
    }
    if needs_palettise {
        if debug {
            println!("[adding palettise]");
        }
        let new_stage = Stage::new("palette", &cur_fmt, ofmt, options)?;
        //cur_fmt = new_stage.fmt_out;
        add_stage!(stages, new_stage);
    }

    if let Some(ref mut head) = stages {
        head.drop_last_tmp();
    }

    Ok(stages)
}

fn swap_plane<T:Copy>(data: &mut [T], stride: usize, h: usize, line0: &mut [T], line1: &mut [T]) {
    let mut doff0 = 0;
    let mut doff1 = stride * (h - 1);
    for _ in 0..h/2 {
        line0.copy_from_slice(&data[doff0..][..stride]);
        line1.copy_from_slice(&data[doff1..][..stride]);
        data[doff1..][..stride].copy_from_slice(line0);
        data[doff0..][..stride].copy_from_slice(line1);
        doff0 += stride;
        doff1 -= stride;
    }
}

/// Flips the picture contents.
pub fn flip_picture(pic: &mut NABufferType) -> ScaleResult<()> {
    match pic {
        NABufferType::Video(ref mut vb) => {
            let ncomp = vb.get_num_components();
            for comp in 0..ncomp {
                let off    = vb.get_offset(comp);
                let stride = vb.get_stride(comp);
                let (_, h) = vb.get_dimensions(comp);
                let data = vb.get_data_mut().unwrap();
                let mut line0 = vec![0; stride];
                let mut line1 = vec![0; stride];
                swap_plane(&mut data[off..], stride, h, line0.as_mut_slice(), line1.as_mut_slice());
            }
        },
        NABufferType::Video16(ref mut vb) => {
            let ncomp = vb.get_num_components().max(1);
            for comp in 0..ncomp {
                let off    = vb.get_offset(comp);
                let stride = vb.get_stride(comp);
                let (_, h) = vb.get_dimensions(comp);
                let data = vb.get_data_mut().unwrap();
                let mut line0 = vec![0; stride];
                let mut line1 = vec![0; stride];
                swap_plane(&mut data[off..], stride, h, line0.as_mut_slice(), line1.as_mut_slice());
            }
        },
        NABufferType::Video32(ref mut vb) => {
            let ncomp = vb.get_num_components().max(1);
            for comp in 0..ncomp {
                let off    = vb.get_offset(comp);
                let stride = vb.get_stride(comp);
                let (_, h) = vb.get_dimensions(comp);
                let data = vb.get_data_mut().unwrap();
                let mut line0 = vec![0; stride];
                let mut line1 = vec![0; stride];
                swap_plane(&mut data[off..], stride, h, line0.as_mut_slice(), line1.as_mut_slice());
            }
        },
        NABufferType::VideoPacked(ref mut vb) => {
            let ncomp = vb.get_num_components();
            for comp in 0..ncomp {
                let off    = vb.get_offset(comp);
                let stride = vb.get_stride(comp);
                let (_, h) = vb.get_dimensions(comp);
                let data = vb.get_data_mut().unwrap();
                let mut line0 = vec![0; stride];
                let mut line1 = vec![0; stride];
                swap_plane(&mut data[off..], stride, h, line0.as_mut_slice(), line1.as_mut_slice());
            }
            if ncomp == 0 && vb.get_stride(0) != 0 {
                let off    = vb.get_offset(0);
                let stride = vb.get_stride(0);
                let (_, h) = vb.get_dimensions(0);
                let data = vb.get_data_mut().unwrap();
                let mut line0 = vec![0; stride];
                let mut line1 = vec![0; stride];
                swap_plane(&mut data[off..], stride, h, line0.as_mut_slice(), line1.as_mut_slice());
            }
        },
        _ => { return Err(ScaleError::InvalidArgument); },
    };
    Ok(())
}

impl NAScale {
    /// Constructs a new `NAScale` instance.
    pub fn new(fmt_in: ScaleInfo, fmt_out: ScaleInfo) -> ScaleResult<Self> {
        let just_convert = (fmt_in.width == fmt_out.width) && (fmt_in.height == fmt_out.height);
        let pipeline = if fmt_in != fmt_out {
                build_pipeline(&fmt_in, &fmt_out, just_convert, &[])?
            } else {
                None
            };
        Ok(Self { fmt_in, fmt_out, just_convert, pipeline })
    }
    /// Constructs a new `NAScale` instance taking into account provided options.
    pub fn new_with_options(fmt_in: ScaleInfo, fmt_out: ScaleInfo, options: &[(String, String)]) -> ScaleResult<Self> {
        let just_convert = (fmt_in.width == fmt_out.width) && (fmt_in.height == fmt_out.height);
        let pipeline = if fmt_in != fmt_out {
                build_pipeline(&fmt_in, &fmt_out, just_convert, options)?
            } else {
                None
            };
        Ok(Self { fmt_in, fmt_out, just_convert, pipeline })
    }
    /// Checks whether requested conversion operation is needed at all.
    pub fn needs_processing(&self) -> bool { self.pipeline.is_some() }
    /// Returns the input image format.
    pub fn get_in_fmt(&self) -> ScaleInfo { self.fmt_in }
    /// Returns the output image format.
    pub fn get_out_fmt(&self) -> ScaleInfo { self.fmt_out }
    /// Performs the image format conversion.
    pub fn convert(&mut self, pic_in: &NABufferType, pic_out: &mut NABufferType) -> ScaleResult<()> {
        let in_info  = pic_in.get_video_info();
        let out_info = pic_out.get_video_info();
        if in_info.is_none() || out_info.is_none() { return Err(ScaleError::InvalidArgument); }
        let in_info  = in_info.unwrap();
        let out_info = out_info.unwrap();
        if self.just_convert &&
                (in_info.get_width() != out_info.get_width() || in_info.get_height() != out_info.get_height()) {
            return Err(ScaleError::InvalidArgument);
        }
        let needs_flip = in_info.is_flipped() ^ out_info.is_flipped();
        check_format(in_info,  &self.fmt_in,  self.just_convert)?;
        check_format(out_info, &self.fmt_out, self.just_convert)?;
        let ret = if let Some(ref mut pipe) = self.pipeline {
                pipe.process(pic_in, pic_out)
            } else {
                copy(pic_in, pic_out);
                Ok(())
            };
        if ret.is_ok() && needs_flip {
            flip_picture(pic_out)?;
        }
        ret
    }
}

#[cfg(test)]
mod test {
    use super::*;

    fn fill_pic(pic: &mut NABufferType, val: u8) {
        if let Some(ref mut buf) = pic.get_vbuf() {
            let data = buf.get_data_mut().unwrap();
            for el in data.iter_mut() { *el = val; }
        } else if let Some(ref mut buf) = pic.get_vbuf16() {
            let data = buf.get_data_mut().unwrap();
            for el in data.iter_mut() { *el = val as u16; }
        } else if let Some(ref mut buf) = pic.get_vbuf32() {
            let data = buf.get_data_mut().unwrap();
            for el in data.iter_mut() { *el = (val as u32) * 0x01010101; }
        }
    }
    #[test]
    fn test_convert() {
        let mut in_pic = alloc_video_buffer(NAVideoInfo::new(1, 1, false, RGB565_FORMAT), 3).unwrap();
        fill_pic(&mut in_pic, 42);
        let mut out_pic = alloc_video_buffer(NAVideoInfo::new(1, 1, false, RGB24_FORMAT), 3).unwrap();
        fill_pic(&mut out_pic, 0);
        let ifmt = get_scale_fmt_from_pic(&in_pic);
        let ofmt = get_scale_fmt_from_pic(&out_pic);
        let mut scaler = NAScale::new(ifmt, ofmt).unwrap();
        scaler.convert(&in_pic, &mut out_pic).unwrap();
        let obuf = out_pic.get_vbuf().unwrap();
        let odata = obuf.get_data();
        assert_eq!(odata[0], 0x0);
        assert_eq!(odata[1], 0x4);
        assert_eq!(odata[2], 0x52);

        let mut in_pic = alloc_video_buffer(NAVideoInfo::new(4, 4, false, RGB24_FORMAT), 3).unwrap();
        fill_pic(&mut in_pic, 42);
        let mut out_pic = alloc_video_buffer(NAVideoInfo::new(4, 4, false, YUV420_FORMAT), 3).unwrap();
        fill_pic(&mut out_pic, 0);
        let ifmt = get_scale_fmt_from_pic(&in_pic);
        let ofmt = get_scale_fmt_from_pic(&out_pic);
        let mut scaler = NAScale::new(ifmt, ofmt).unwrap();
        scaler.convert(&in_pic, &mut out_pic).unwrap();
        let obuf = out_pic.get_vbuf().unwrap();
        let yoff = obuf.get_offset(0);
        let uoff = obuf.get_offset(1);
        let voff = obuf.get_offset(2);
        let odata = obuf.get_data();
        assert_eq!(odata[yoff], 42);
        assert!(((odata[uoff] ^ 0x80) as i8).abs() <= 1);
        assert!(((odata[voff] ^ 0x80) as i8).abs() <= 1);
        let mut scaler = NAScale::new(ofmt, ifmt).unwrap();
        scaler.convert(&out_pic, &mut in_pic).unwrap();
        let obuf = in_pic.get_vbuf().unwrap();
        let odata = obuf.get_data();
        assert_eq!(odata[0], 42);
    }
    #[test]
    fn test_scale() {
        let mut in_pic = alloc_video_buffer(NAVideoInfo::new(2, 2, false, RGB565_FORMAT), 3).unwrap();
        fill_pic(&mut in_pic, 42);
        let mut out_pic = alloc_video_buffer(NAVideoInfo::new(3, 3, false, RGB565_FORMAT), 3).unwrap();
        fill_pic(&mut out_pic, 0);
        let ifmt = get_scale_fmt_from_pic(&in_pic);
        let ofmt = get_scale_fmt_from_pic(&out_pic);
        let mut scaler = NAScale::new(ifmt, ofmt).unwrap();
        scaler.convert(&in_pic, &mut out_pic).unwrap();
        let obuf = out_pic.get_vbuf16().unwrap();
        let odata = obuf.get_data();
        assert_eq!(odata[0], 42);
    }
    #[test]
    fn test_scale_and_convert() {
        let mut in_pic = alloc_video_buffer(NAVideoInfo::new(7, 3, false, RGB565_FORMAT), 3).unwrap();
        fill_pic(&mut in_pic, 42);
        let mut out_pic = alloc_video_buffer(NAVideoInfo::new(4, 4, false, YUV420_FORMAT), 3).unwrap();
        fill_pic(&mut out_pic, 0);
        let ifmt = get_scale_fmt_from_pic(&in_pic);
        let ofmt = get_scale_fmt_from_pic(&out_pic);
        let mut scaler = NAScale::new(ifmt, ofmt).unwrap();
        scaler.convert(&in_pic, &mut out_pic).unwrap();
        let obuf = out_pic.get_vbuf().unwrap();
        let yoff = obuf.get_offset(0);
        let uoff = obuf.get_offset(1);
        let voff = obuf.get_offset(2);
        let odata = obuf.get_data();
        assert_eq!(odata[yoff], 11);
        assert_eq!(odata[uoff], 162);
        assert_eq!(odata[voff], 118);
    }
    #[test]
    fn test_scale_and_convert_to_pal() {
        let mut in_pic = alloc_video_buffer(NAVideoInfo::new(7, 3, false, YUV420_FORMAT), 3).unwrap();
        fill_pic(&mut in_pic, 142);
        let mut out_pic = alloc_video_buffer(NAVideoInfo::new(4, 4, false, PAL8_FORMAT), 0).unwrap();
        fill_pic(&mut out_pic, 0);
        let ifmt = get_scale_fmt_from_pic(&in_pic);
        let ofmt = get_scale_fmt_from_pic(&out_pic);
        let mut scaler = NAScale::new(ifmt, ofmt).unwrap();
        scaler.convert(&in_pic, &mut out_pic).unwrap();
        let obuf = out_pic.get_vbuf().unwrap();
        let dataoff = obuf.get_offset(0);
        let paloff  = obuf.get_offset(1);
        let odata = obuf.get_data();
        assert_eq!(odata[dataoff], 0);
        assert_eq!(odata[paloff], 157);
        assert_eq!(odata[paloff + 1], 129);
        assert_eq!(odata[paloff + 2], 170);
    }
    #[test]
    fn test_scale_modes() {
        const IN_DATA: [[u8; 6]; 2] = [
            [0xFF, 0xC0, 0x40, 0x00, 0x40, 0xC0],
            [0x00, 0x40, 0xC0, 0xFF, 0xC0, 0x40]
        ];
        const TEST_DATA: &[(&str, [[u8; 9]; 3])] = &[
            ("nn",
               [[0xFF, 0xC0, 0x40, 0xFF, 0xC0, 0x40, 0x00, 0x40, 0xC0],
                [0xFF, 0xC0, 0x40, 0xFF, 0xC0, 0x40, 0x00, 0x40, 0xC0],
                [0x00, 0x40, 0xC0, 0x00, 0x40, 0xC0, 0xFF, 0xC0, 0x40]]),
            ("bilin",
               [[0xFF, 0xC0, 0x40, 0x55, 0x6A, 0x95, 0x00, 0x40, 0xC0],
                [0x55, 0x6A, 0x95, 0x8D, 0x86, 0x78, 0xAA, 0x95, 0x6A],
                [0x00, 0x40, 0xC0, 0xAA, 0x95, 0x6A, 0xFF, 0xC0, 0x40]]),
            ("bicubic",
               [[0xFF, 0xC0, 0x40, 0x4B, 0x65, 0x9A, 0x00, 0x36, 0xC9],
                [0x4B, 0x65, 0x9A, 0x94, 0x8A, 0x74, 0xB3, 0x9D, 0x61],
                [0x00, 0x36, 0xC9, 0xBA, 0x9D, 0x61, 0xFF, 0xD3, 0x2B]]),
            ("lanczos",
               [[0xFF, 0xC0, 0x40, 0x4C, 0x66, 0x98, 0x00, 0x31, 0xCD],
                [0x4C, 0x66, 0x98, 0x91, 0x88, 0x74, 0xB1, 0x9D, 0x5F],
                [0x00, 0x31, 0xCD, 0xBB, 0x9D, 0x5F, 0xFF, 0xDD, 0x1E]]),
            ("lanczos2",
               [[0xFF, 0xC0, 0x40, 0x4F, 0x68, 0x9B, 0x00, 0x35, 0xCD],
                [0x4F, 0x68, 0x9B, 0x96, 0x8D, 0x79, 0xB3, 0xA0, 0x64],
                [0x00, 0x35, 0xCD, 0xBE, 0xA1, 0x65, 0xFF, 0xDC, 0x28]]),
        ];

        let in_pic = alloc_video_buffer(NAVideoInfo::new(2, 2, false, RGB24_FORMAT), 3).unwrap();
        if let Some(ref mut vbuf) = in_pic.get_vbuf() {
            let stride = vbuf.get_stride(0);
            let data = vbuf.get_data_mut().unwrap();
            for (dline, rline) in data.chunks_mut(stride).zip(IN_DATA.iter()) {
                dline[..6].copy_from_slice(rline);
            }
        } else {
            panic!("wrong format");
        }
        let mut out_pic = alloc_video_buffer(NAVideoInfo::new(3, 3, false, RGB24_FORMAT), 3).unwrap();
        let ifmt = get_scale_fmt_from_pic(&in_pic);
        let ofmt = get_scale_fmt_from_pic(&out_pic);
        for (method, ref_data) in TEST_DATA.iter() {
            fill_pic(&mut out_pic, 0);
            let mut scaler = NAScale::new_with_options(ifmt, ofmt, &[("scaler".to_string(), method.to_string())]).unwrap();
            scaler.convert(&in_pic, &mut out_pic).unwrap();
            let obuf = out_pic.get_vbuf().unwrap();
            let ostride = obuf.get_stride(0);
            let odata = obuf.get_data();
            for (oline, rline) in odata.chunks(ostride).zip(ref_data.iter()) {
                for (&a, &b) in oline.iter().zip(rline.iter()) {
                    assert_eq!(a, b);
                }
            }
        }
    }
}
