use std::collections::VecDeque;
use nihav_core::io::bitwriter::*;

const BUNDLE_LEN_BITS: u8 = 13;
const MAX_BUNDLE_LEN: usize = 1 << BUNDLE_LEN_BITS;

#[derive(Default)]
struct Bundle<T:Copy> {
    bits:   u8,
    data:   VecDeque<(usize, Vec<T>)>,
    row:    usize,
    tmp:    Vec<T>,
    end:    bool,
    last_w: usize,
}

impl<T:Copy> Bundle<T> {
    fn reset(&mut self, bits: u8) {
        self.bits = bits;
        self.end = false;
        self.last_w = 0;
    }
    fn new_row(&mut self, row: usize) {
        self.row = row;
    }
    fn push(&mut self, val: T) {
        self.tmp.push(val);
    }
    fn push_all(&mut self, slc: &[T]) {
        self.tmp.extend_from_slice(slc);
    }
    fn end_row(&mut self) {
        if !self.tmp.is_empty() {
            let mut tmp = Vec::new();
            std::mem::swap(&mut tmp, &mut self.tmp);
            self.data.push_back((self.row, tmp));
        }
    }
}

trait IntoU32 {
    fn into_u32(self) -> u32;
}

impl IntoU32 for u8 {
    fn into_u32(self) -> u32 { u32::from(self) }
}

impl IntoU32 for i8 {
    fn into_u32(self) -> u32 { (self + 16) as u32 }
}

impl IntoU32 for u16 {
    fn into_u32(self) -> u32 { u32::from(self) }
}

impl IntoU32 for i16 {
    fn into_u32(self) -> u32 { (self + 1024) as u32 }
}

impl Bundle<(u16, u8)> {
    fn write(&mut self, bw: &mut BitWriter, cur_row: usize) {
        if !self.data.is_empty() && self.data[0].0 == cur_row {
            let (_, row_data) = self.data.pop_front().unwrap();
            for &(bits, len) in row_data.iter() {
                bw.write(u32::from(bits), len);
            }
        }
    }
}

impl<T: Copy+IntoU32> Bundle<T> {
    fn write(&mut self, bw: &mut BitWriter, cur_row: usize) {
        if !self.end && cur_row == self.last_w {
            let mut num_out = 0;
            let mut len_out = 0;
            for (_, row) in self.data.iter() {
                if len_out + row.len() < MAX_BUNDLE_LEN {
                    len_out += row.len();
                    num_out += 1;
                } else {
                    break;
                }
            }

            bw.write(len_out as u32, BUNDLE_LEN_BITS);
            if len_out == 0 {
                self.end = true;
                return;
            }
            for _ in 0..num_out {
                let (row_no, row_data) = self.data.pop_front().unwrap();
                self.last_w = row_no + 1;
                for &el in row_data.iter() {
                    bw.write(el.into_u32(), self.bits);
                }
            }
        }
    }
}

#[derive(Default)]
pub struct Bundles {
    btype:      Bundle<u8>,
    colors:     Bundle<u8>,
    pattern:    Bundle<u8>,
    xoff:       Bundle<i8>,
    yoff:       Bundle<i8>,
    intradc:    Bundle<u16>,
    interdc:    Bundle<i16>,
    intraq:     Bundle<u8>,
    interq:     Bundle<u8>,
    nresidues:  Bundle<u8>,
    other:      Bundle<(u16, u8)>,
}

macro_rules! whole_bundle {
    ($self:expr, $func:ident) => {
        $self.btype.$func();
        $self.colors.$func();
        $self.pattern.$func();
        $self.xoff.$func();
        $self.yoff.$func();
        $self.intradc.$func();
        $self.interdc.$func();
        $self.intraq.$func();
        $self.interq.$func();
        $self.nresidues.$func();
        $self.other.$func();
    };
    ($self:expr, $func:ident, $($args:expr),*) => {
        $self.btype.$func($($args),*);
        $self.colors.$func($($args),*);
        $self.pattern.$func($($args),*);
        $self.xoff.$func($($args),*);
        $self.yoff.$func($($args),*);
        $self.intradc.$func($($args),*);
        $self.interdc.$func($($args),*);
        $self.intraq.$func($($args),*);
        $self.interq.$func($($args),*);
        $self.nresidues.$func($($args),*);
        $self.other.$func($($args),*);
    }
}

impl Bundles {
    pub fn reset(&mut self) {
        self.btype.reset(4);
        self.colors.reset(8);
        self.pattern.reset(8);
        self.xoff.reset(5);
        self.yoff.reset(5);
        self.intradc.reset(11);
        self.interdc.reset(11);
        self.intraq.reset(4);
        self.interq.reset(4);
        self.nresidues.reset(7);
    }
    pub fn add_block_type(&mut self, btype: u8) {
        self.btype.push(btype);
    }
    pub fn write(&mut self, bw: &mut BitWriter, row: usize) {
        whole_bundle!(self, write, bw, row);
    }
    pub fn new_row(&mut self, row: usize) {
        whole_bundle!(self, new_row, row);
    }
    pub fn end_row(&mut self) {
        whole_bundle!(self, end_row);
    }

    pub fn can_fit_raw_block(&self) -> bool {
        self.colors.data.len() < MAX_BUNDLE_LEN - 1 - 64
    }
    pub fn add_tokens(&mut self, tokens: &BlockTokens) {
        self.colors.push_all(&tokens.colors);
        self.pattern.push_all(&tokens.pattern);
        self.xoff.push_all(&tokens.xoff);
        self.yoff.push_all(&tokens.yoff);
        self.intradc.push_all(&tokens.intradc);
        self.interdc.push_all(&tokens.interdc);
        self.intraq.push_all(&tokens.intraq);
        self.interq.push_all(&tokens.interq);
        self.nresidues.push_all(&tokens.nresidues);
        self.other.push_all(&tokens.other);
    }
}

#[derive(Default)]
pub struct BlockTokens {
    pub colors:     Vec<u8>,
    pub pattern:    Vec<u8>,
    pub xoff:       Vec<i8>,
    pub yoff:       Vec<i8>,
    pub intradc:    Vec<u16>,
    pub interdc:    Vec<i16>,
    pub intraq:     Vec<u8>,
    pub interq:     Vec<u8>,
    pub nresidues:  Vec<u8>,
    pub other:      Vec<(u16, u8)>,
}

impl BlockTokens {
    pub fn new() -> Self { Self::default() }
    pub fn clear(&mut self) {
        self.colors.clear();
        self.pattern.clear();
        self.xoff.clear();
        self.yoff.clear();
        self.intradc.clear();
        self.interdc.clear();
        self.intraq.clear();
        self.interq.clear();
        self.nresidues.clear();
        self.other.clear();
    }
    pub fn bits(&self, is_b: bool) -> usize {
        if is_b {
            self.colors.len()    * 8 +
            self.pattern.len()   * 8 +
            self.xoff.len()      * 5 +
            self.yoff.len()      * 5 +
            self.intradc.len()   * 11 +
            self.interdc.len()   * 11 +
            self.intraq.len()    * 4 +
            self.interq.len()    * 4 +
            self.nresidues.len() * 7 +
            self.other.iter().fold(0usize, |acc, &(_, len)| acc + usize::from(len))
        } else {
unimplemented!()
        }
    }
}
