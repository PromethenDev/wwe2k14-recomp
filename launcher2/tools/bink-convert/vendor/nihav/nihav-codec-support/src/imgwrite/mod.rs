//! Simple PNM image writers for RGB and YUV images.
use std::fs::File;
use nihav_core::frame::{NABufferType, NAFrameRef, NAFrame, alloc_video_buffer};
use nihav_core::io::byteio::*;
use nihav_core::scale::*;
use nihav_core::formats::YUV420_FORMAT;

/// The error type for image writing operations.
#[derive(Clone,Copy,Debug,PartialEq)]
pub enum ImgWriteError {
    /// File open error.
    OpenError,
    /// Invalid input format (e.g. audio).
    InvalidInput,
    /// Unsupported input format.
    NotImplemented,
    /// Write error.
    WriteError,
}

/// A specialised `Return` type for image writing operations.
pub type ImgWriteResult<T> = Result<T, ImgWriteError>;

impl From<ByteIOError> for ImgWriteError {
    fn from(_: ByteIOError) -> ImgWriteError { ImgWriteError::WriteError }
}

/// Writes PGMYUV for input frame.
pub fn write_pgmyuv(name: &str, frm: NAFrameRef) -> ImgWriteResult<()> {
    if let NABufferType::None = frm.get_buffer() { return Ok(()); }
    let ofile = File::create(name).map_err(|_| ImgWriteError::OpenError)?;
    let mut fw = FileWriter::new_write(ofile);
    match frm.get_buffer() {
        NABufferType::Video(ref buf) | NABufferType::VideoPacked(ref buf) => {
            let is_flipped = buf.get_info().is_flipped();
            let (w, h) = buf.get_dimensions(0);
            let (w2, h2) = buf.get_dimensions(1);
            let full_w = w2 * 2 > w;
            let has_alpha = buf.get_info().get_format().has_alpha();
            let mut tot_h = h + h2;
            if has_alpha {
                tot_h += h;
            }
            if full_w {
                tot_h += h2;
            }
            let hdr = format!("P5\n{} {}\n255\n", w, tot_h);
            fw.write_buf(hdr.as_bytes())?;
            let dta = buf.get_data();
            let ls = buf.get_stride(0);
            let pad: Vec<u8> = vec![0xFF; if !full_w { (w - w2 * 2) / 2 } else { w - w2 } ];
            if !is_flipped {
                let ylines = dta.chunks(ls).take(h);
                for line in ylines {
                    fw.write_buf(&line[..w])?;
                }
            } else {
                let ylines = dta[..h * ls].chunks(ls).rev();
                for line in ylines {
                    fw.write_buf(&line[..w])?;
                }
            }
            let base1 = buf.get_offset(1);
            let stride1 = buf.get_stride(1);
            let base2 = buf.get_offset(2);
            let stride2 = buf.get_stride(2);
            let u = &dta[base1..][..h2*stride1];
            let v = &dta[base2..][..h2*stride2];
            let has_chroma = stride1 > 0 && stride2 > 0;
            if !full_w && has_chroma {
                if !is_flipped {
                    for (uline, vline) in u.chunks(stride1).zip(v.chunks(stride2)) {
                        fw.write_buf(&uline[..w2])?;
                        fw.write_buf(pad.as_slice())?;

                        fw.write_buf(&vline[..w2])?;
                        fw.write_buf(pad.as_slice())?;
                    }
                } else {
                    for (uline, vline) in u.chunks(stride1).rev().zip(v.chunks(stride2).rev()) {
                        fw.write_buf(&uline[..w2])?;
                        fw.write_buf(pad.as_slice())?;

                        fw.write_buf(&vline[..w2])?;
                        fw.write_buf(pad.as_slice())?;
                    }
                }
            } else if has_chroma {
                if !is_flipped {
                    for uline in u.chunks(stride1) {
                        fw.write_buf(&uline[..w2])?;
                        fw.write_buf(pad.as_slice())?;
                    }
                    for vline in v.chunks(stride2) {
                        fw.write_buf(&vline[..w2])?;
                        fw.write_buf(pad.as_slice())?;
                    }
                } else {
                    for uline in u.chunks(stride1).rev() {
                        fw.write_buf(&uline[..w2])?;
                        fw.write_buf(pad.as_slice())?;
                    }
                    for vline in v.chunks(stride2).rev() {
                        fw.write_buf(&vline[..w2])?;
                        fw.write_buf(pad.as_slice())?;
                    }
                }
            }
            if has_alpha {
                let ls = buf.get_stride(3);
                if !is_flipped {
                    let alines = dta[buf.get_offset(3)..].chunks(ls).take(h);
                    for line in alines {
                        fw.write_buf(&line[..w])?;
                    }
                } else {
                    let alines = dta[buf.get_offset(3)..].chunks(ls).take(h).rev();
                    for line in alines {
                        fw.write_buf(&line[..w])?;
                    }
                }
            }
        },
        NABufferType::Video16(ref buf) => {
            let is_flipped = buf.get_info().is_flipped();
            let (w, h) = buf.get_dimensions(0);
            let (w2, h2) = buf.get_dimensions(1);
            let full_w = w2 * 2 > w;
            let has_alpha = buf.get_info().get_format().has_alpha();
            let depth = buf.get_info().get_format().get_max_depth();
            let mut tot_h = h + h2;
            if has_alpha {
                tot_h += h;
            }
            if full_w {
                tot_h += h2;
            }
            let hdr = format!("P5\n{} {}\n{}\n", w, tot_h, (1 << depth) - 1);
            fw.write_buf(hdr.as_bytes())?;
            let dta = buf.get_data();
            let ls = buf.get_stride(0);
            let pad: Vec<u8> = vec![0xFF; if !full_w { w - w2 * 2 } else { (w - w2) * 2 } ];
            if !is_flipped {
                let ylines = dta.chunks(ls).take(h);
                for line in ylines {
                    fw.write_u16be_arr(&line[..w])?;
                }
            } else {
                let ylines = dta[..h * ls].chunks(ls).rev();
                for line in ylines {
                    fw.write_u16be_arr(&line[..w])?;
                }
            }
            let base1 = buf.get_offset(1);
            let stride1 = buf.get_stride(1);
            let base2 = buf.get_offset(2);
            let stride2 = buf.get_stride(2);
            let u = &dta[base1..][..h2*stride1];
            let v = &dta[base2..][..h2*stride2];
            let has_chroma = stride1 > 0 && stride2 > 0;
            if !full_w && has_chroma {
                if !is_flipped {
                    for (uline, vline) in u.chunks(stride1).zip(v.chunks(stride2)) {
                        fw.write_u16be_arr(&uline[..w2])?;
                        fw.write_buf(pad.as_slice())?;

                        fw.write_u16be_arr(&vline[..w2])?;
                        fw.write_buf(pad.as_slice())?;
                    }
                } else {
                    for (uline, vline) in u.chunks(stride1).rev().zip(v.chunks(stride2).rev()) {
                        fw.write_u16be_arr(&uline[..w2])?;
                        fw.write_buf(pad.as_slice())?;

                        fw.write_u16be_arr(&vline[..w2])?;
                        fw.write_buf(pad.as_slice())?;
                    }
                }
            } else if has_chroma {
                if !is_flipped {
                    for uline in u.chunks(stride1) {
                        fw.write_u16be_arr(&uline[..w2])?;
                        fw.write_buf(pad.as_slice())?;
                    }
                    for vline in v.chunks(stride2) {
                        fw.write_u16be_arr(&vline[..w2])?;
                        fw.write_buf(pad.as_slice())?;
                    }
                } else {
                    for uline in u.chunks(stride1).rev() {
                        fw.write_u16be_arr(&uline[..w2])?;
                        fw.write_buf(pad.as_slice())?;
                    }
                    for vline in v.chunks(stride2).rev() {
                        fw.write_u16be_arr(&vline[..w2])?;
                        fw.write_buf(pad.as_slice())?;
                    }
                }
            }
            if has_alpha {
                let ls = buf.get_stride(3);
                if !is_flipped {
                    let alines = dta[buf.get_offset(3)..].chunks(ls).take(h);
                    for line in alines {
                        fw.write_u16be_arr(&line[..w])?;
                    }
                } else {
                    let alines = dta[buf.get_offset(3)..].chunks(ls).take(h).rev();
                    for line in alines {
                        fw.write_u16be_arr(&line[..w])?;
                    }
                }
            }
        },
        NABufferType::Video32(ref _buf) => {
            return Err(ImgWriteError::NotImplemented);
        },
        NABufferType::None => unreachable!(),
        _ => return Err(ImgWriteError::InvalidInput),
    }
    fw.flush()?;
    Ok(())
}

/// Writes output PPM for input paletted input frame.
pub fn write_palppm(name: &str, frm: NAFrameRef) -> ImgWriteResult<()> {
    if let NABufferType::None = frm.get_buffer() { return Ok(()); }
    let ofile = File::create(name).map_err(|_| ImgWriteError::OpenError)?;
    let mut fw = FileWriter::new_write(ofile);
    let buf = frm.get_buffer().get_vbuf().unwrap();
    let (w, h) = buf.get_dimensions(0);
    let paloff = buf.get_offset(1);
    let hdr = format!("P6\n{} {}\n255\n", w, h);
    fw.write_buf(hdr.as_bytes())?;
    let dta = buf.get_data();
    let ls = buf.get_stride(0);
    let offs: [usize; 3] = [
            buf.get_info().get_format().get_chromaton(0).unwrap().get_offset() as usize,
            buf.get_info().get_format().get_chromaton(1).unwrap().get_offset() as usize,
            buf.get_info().get_format().get_chromaton(2).unwrap().get_offset() as usize
        ];
    let flipped = buf.get_info().is_flipped();
    let mut idx  = if !flipped { 0 } else { ls * h };
    let mut line: Vec<u8> = vec![0; w * 3];
    for _ in 0..h {
        if flipped {
            idx -= ls;
        }
        let src = &dta[idx..(idx+w)];
        for x in 0..w {
            let pix = src[x] as usize;
            line[x * 3 + 0] = dta[paloff + pix * 3 + offs[0]];
            line[x * 3 + 1] = dta[paloff + pix * 3 + offs[1]];
            line[x * 3 + 2] = dta[paloff + pix * 3 + offs[2]];
        }
        fw.write_buf(line.as_slice())?;
        if !flipped {
            idx += ls;
        }
    }
    fw.flush()?;
    Ok(())
}

/// Writes PPM file for RGB input.
pub fn write_rgbppm(name: &str, frm: NAFrameRef) -> ImgWriteResult<()> {
    if let NABufferType::None = frm.get_buffer() { return Ok(()); }
    let ofile = File::create(name).map_err(|_| ImgWriteError::OpenError)?;
    let mut fw = FileWriter::new_write(ofile);
    let info = frm.get_buffer().get_video_info().unwrap();
    let flipped = info.is_flipped();
    match frm.get_buffer() {
        NABufferType::Video(ref buf) | NABufferType::VideoPacked(ref buf) => {
            let (w, h) = buf.get_dimensions(0);
            let hdr = format!("P6\n{} {}\n255\n", w, h);
            fw.write_buf(hdr.as_bytes())?;
            let dta = buf.get_data();
            let stride = buf.get_stride(0);
            let offs: [usize; 3] = [
                    info.get_format().get_chromaton(0).unwrap().get_offset() as usize,
                    info.get_format().get_chromaton(1).unwrap().get_offset() as usize,
                    info.get_format().get_chromaton(2).unwrap().get_offset() as usize
                ];
            let step = info.get_format().get_elem_size() as usize;
            let mut line: Vec<u8> = vec![0; w * 3];
            if !flipped {
                for src in dta.chunks(stride) {
                    for x in 0..w {
                        line[x * 3 + 0] = src[x * step + offs[0]];
                        line[x * 3 + 1] = src[x * step + offs[1]];
                        line[x * 3 + 2] = src[x * step + offs[2]];
                    }
                    fw.write_buf(line.as_slice())?;
                }
            } else {
                for src in dta[..stride * h].chunks(stride).rev() {
                    for x in 0..w {
                        line[x * 3 + 0] = src[x * step + offs[0]];
                        line[x * 3 + 1] = src[x * step + offs[1]];
                        line[x * 3 + 2] = src[x * step + offs[2]];
                    }
                    fw.write_buf(line.as_slice())?;
                }
            }
        },
        NABufferType::Video16(ref buf) => {
            let (w, h) = buf.get_dimensions(0);
            let hdr = format!("P6\n{} {}\n255\n", w, h);
            fw.write_buf(hdr.as_bytes())?;
            let dta = buf.get_data();
            let stride = buf.get_stride(0);
            let depths: [u8; 3] = [
                    info.get_format().get_chromaton(0).unwrap().get_depth(),
                    info.get_format().get_chromaton(1).unwrap().get_depth(),
                    info.get_format().get_chromaton(2).unwrap().get_depth()
                ];
            let masks: [u16; 3] = [
                    (1 << depths[0]) - 1,
                    (1 << depths[1]) - 1,
                    (1 << depths[2]) - 1
                ];
            let shifts: [u8; 3] = [
                    info.get_format().get_chromaton(0).unwrap().get_shift(),
                    info.get_format().get_chromaton(1).unwrap().get_shift(),
                    info.get_format().get_chromaton(2).unwrap().get_shift()
                ];
            let mut line: Vec<u8> = vec![0; w * 3];
            if !flipped {
                for src in dta.chunks(stride) {
                    for x in 0..w {
                        let elem = src[x];
                        let r = ((elem >> shifts[0]) & masks[0]) << (8 - depths[0]);
                        let g = ((elem >> shifts[1]) & masks[1]) << (8 - depths[1]);
                        let b = ((elem >> shifts[2]) & masks[2]) << (8 - depths[2]);
                        line[x * 3 + 0] = r as u8;
                        line[x * 3 + 1] = g as u8;
                        line[x * 3 + 2] = b as u8;
                    }
                    fw.write_buf(line.as_slice())?;
                }
            } else {
                for src in dta[..h * stride].chunks(stride).rev() {
                    for x in 0..w {
                        let elem = src[x];
                        let r = ((elem >> shifts[0]) & masks[0]) << (8 - depths[0]);
                        let g = ((elem >> shifts[1]) & masks[1]) << (8 - depths[1]);
                        let b = ((elem >> shifts[2]) & masks[2]) << (8 - depths[2]);
                        line[x * 3 + 0] = r as u8;
                        line[x * 3 + 1] = g as u8;
                        line[x * 3 + 2] = b as u8;
                    }
                    fw.write_buf(line.as_slice())?;
                }
            }
        },
        NABufferType::Video32(ref _buf) => return Err(ImgWriteError::NotImplemented),
        _ => return Err(ImgWriteError::InvalidInput),
    }
    fw.flush()?;
    Ok(())
}

/// Writes PNM file with a format depending on input format.
pub fn write_pnm(pfx: &str, strno: usize, num: u64, frm: NAFrameRef) -> ImgWriteResult<()> {
    if let NABufferType::None = frm.get_buffer() { return Ok(()); }

    let vinfo = frm.get_buffer().get_video_info().unwrap();
    if vinfo.get_format().is_paletted() {
        let name = format!("{}{:02}_{:06}.ppm", pfx, strno, num);
        write_palppm(name.as_str(), frm)
    } else if vinfo.get_format().get_model().is_yuv() {
        let name = format!("{}{:02}_{:06}.pgm", pfx, strno, num);
        if vinfo.get_format().is_unpacked() {
            write_pgmyuv(name.as_str(), frm)
        } else {
            let mut dst_vinfo = vinfo;
            dst_vinfo.format = YUV420_FORMAT;
            let mut cvt_buf = alloc_video_buffer(dst_vinfo, 2).unwrap();
            let buf = frm.get_buffer();

            let ifmt = get_scale_fmt_from_pic(&buf);
            let ofmt = get_scale_fmt_from_pic(&cvt_buf);
            let mut scaler = NAScale::new(ifmt, ofmt).unwrap();
            scaler.convert(&buf, &mut cvt_buf).unwrap();
            let frm = NAFrame::new(frm.get_time_information(), frm.frame_type, frm.key, frm.get_info(), cvt_buf);
            write_pgmyuv(name.as_str(), frm.into_ref())
        }
    } else if vinfo.get_format().get_model().is_rgb() {
        let name = format!("{}{:02}_{:06}.ppm", pfx, strno, num);
        write_rgbppm(name.as_str(), frm)
    } else {
        Err(ImgWriteError::NotImplemented)
    }
}

