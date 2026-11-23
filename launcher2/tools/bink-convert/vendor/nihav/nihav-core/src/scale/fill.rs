use super::*;
use super::kernel::Kernel;

struct FillKernel {}

impl FillKernel {
    fn new() -> Self { Self{} }
}

impl Kernel for FillKernel {
    fn init(&mut self, in_fmt: &ScaleInfo, dest_fmt: &ScaleInfo, _options: &[(String, String)]) -> ScaleResult<NABufferType> {
        let mut dfmt = dest_fmt.fmt;
        for (i, chr) in dfmt.comp_info.iter_mut().flatten().enumerate() {
            chr.packed = false;
            chr.comp_offs = i as u8;
            chr.next_elem = 0;
        }
        let res = alloc_video_buffer(NAVideoInfo::new(in_fmt.width, in_fmt.height, false, dfmt), 3);
        if res.is_err() { return Err(ScaleError::AllocError); }
        Ok(res.unwrap())
    }
    fn process(&mut self, pic_in: &NABufferType, pic_out: &mut NABufferType) {
        if let Some(ref vbuf) = pic_in.get_vbuf() {
            if let Some(ref mut dbuf) = pic_out.get_vbuf() {
                let ifmt = vbuf.get_info().format;
                let ofmt = dbuf.get_info().format;

                let num_icore = usize::from(ifmt.components) - if ifmt.alpha { 1 } else { 0 };
                let num_ocore = usize::from(ofmt.components) - if ofmt.alpha { 1 } else { 0 };

                for plane_no in 0..num_icore.min(num_ocore) {
                    let splane_off = vbuf.get_offset(plane_no);
                    let splane_stride = vbuf.get_stride(plane_no);
                    let src = vbuf.get_data();

                    let dplane_off = dbuf.get_offset(plane_no);
                    let dplane_stride = dbuf.get_stride(plane_no);
                    let (width, height) = dbuf.get_dimensions(plane_no);
                    let dst = dbuf.get_data_mut().unwrap();

                    for (dline, sline) in dst[dplane_off..].chunks_mut(dplane_stride)
                            .zip(src[splane_off..].chunks(splane_stride)).take(height) {
                        dline[..width].copy_from_slice(&sline[..width]);
                    }
                }
                if num_icore < num_ocore {
                    let fill_val = match ofmt.model {
                            ColourModel::RGB(_) => 0x00,
                            ColourModel::YUV(_) => 0x80,
                            _ => unimplemented!(),
                        };
                    for plane_no in num_icore..num_ocore {
                        let plane_off = dbuf.get_offset(plane_no);
                        let plane_stride = dbuf.get_stride(plane_no);
                        let (width, height) = dbuf.get_dimensions(plane_no);
                        let dst = dbuf.get_data_mut().unwrap();

                        for line in dst[plane_off..].chunks_mut(plane_stride).take(height) {
                            for el in line.iter_mut().take(width) {
                                *el = fill_val;
                            }
                        }
                    }
                }
                if !ifmt.alpha && ofmt.alpha {
                    let alpha_plane = usize::from(ofmt.components) - 1;
                    let alpha_off = dbuf.get_offset(alpha_plane);
                    let alpha_stride = dbuf.get_stride(alpha_plane);
                    let (width, height) = dbuf.get_dimensions(alpha_plane);
                    let dst = dbuf.get_data_mut().unwrap();

                    for line in dst[alpha_off..].chunks_mut(alpha_stride).take(height) {
                        for el in line.iter_mut().take(width) {
                            *el = 0xFF;
                        }
                    }
                }
            } else {
                unreachable!();
            }
        } else {
            unimplemented!();
        }
    }
}

pub fn create_fill() -> Box<dyn Kernel> {
    Box::new(FillKernel::new())
}
