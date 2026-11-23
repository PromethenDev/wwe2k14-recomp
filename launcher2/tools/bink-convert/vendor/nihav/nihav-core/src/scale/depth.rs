use super::*;
use super::kernel::Kernel;

#[derive(Default)]
struct ShallowKernel {
}

impl ShallowKernel {
    fn new() -> Self { Self::default() }
}

impl Kernel for ShallowKernel {
    fn init(&mut self, in_fmt: &ScaleInfo, dest_fmt: &ScaleInfo, _options: &[(String, String)]) -> ScaleResult<NABufferType> {
        let mut wrk_fmt = in_fmt.fmt;
        for (comp, tcomp) in wrk_fmt.comp_info.iter_mut().zip(dest_fmt.fmt.comp_info.iter()) {
            if let (Some(ref mut wcomp), Some(ref tgtcomp)) = (comp, tcomp) {
                wcomp.depth = tgtcomp.depth;
            }
        }
        let res = alloc_video_buffer(NAVideoInfo::new(in_fmt.width, in_fmt.height, false, wrk_fmt), 3);
        if res.is_err() { return Err(ScaleError::AllocError); }
        Ok(res.unwrap())
    }
    fn process(&mut self, pic_in: &NABufferType, pic_out: &mut NABufferType) {
        if let (Some(ref buf), Some(ref mut dbuf)) = (pic_in.get_vbuf16(), pic_out.get_vbuf()) {
            let ncomps = buf.get_info().get_format().get_num_comp();
            for comp in 0..ncomps {
                let ioff = buf.get_offset(comp);
                let istride = buf.get_stride(comp);
                let idepth = buf.get_info().get_format().get_chromaton(comp).unwrap().get_depth();
                let ddepth = dbuf.get_info().get_format().get_chromaton(comp).unwrap().get_depth();
                let dshift = idepth - ddepth;
                let (w, h) = dbuf.get_dimensions(comp);
                let sdata = buf.get_data();
                let sdata = &sdata[ioff..];
                let doff = dbuf.get_offset(comp);
                let dstride = dbuf.get_stride(comp);
                let ddata = dbuf.get_data_mut().unwrap();
                for (sline, dline) in sdata.chunks(istride).zip(ddata[doff..].chunks_mut(dstride)).take(h) {
                    for (&src, dst) in sline.iter().zip(dline.iter_mut()).take(w) {
                        *dst = (src >> dshift) as u8;
                    }
                }
            }
        } else {
            unimplemented!("shallower not on 16->8");
        }
    }
}

pub fn create_shallow() -> Box<dyn Kernel> {
    Box::new(ShallowKernel::new())
}
