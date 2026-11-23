use crate::frame::*;
use super::{ScaleInfo, ScaleResult};

pub trait Kernel {
    fn init(&mut self, in_fmt: &ScaleInfo, dest_fmt: &ScaleInfo, options: &[(String, String)]) -> ScaleResult<NABufferType>;
    fn process(&mut self, pic_in: &NABufferType, pic_out: &mut NABufferType);
}
