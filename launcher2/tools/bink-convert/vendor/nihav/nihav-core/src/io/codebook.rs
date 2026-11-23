//! Codebook support for bitstream reader.
//!
//! Codebook is a set of unique bit strings and values assigned to them.
//! Since there are many ways to define codebook, this implementation employs [`CodebookDescReader`] trait to provide codebook generator with the codes.
//! Please also pay attention to the codebook creation mode: if bitstream reader reads bits starting from most significant bit first then you should use [`MSB`] mode and [`LSB`] mode otherwise.
//!
//! # Examples
//!
//! Create a codebook from arrays with codeword descriptions:
//! ```
//! use nihav_core::io::codebook::{ShortCodebookDesc, ShortCodebookDescReader, Codebook, CodebookMode};
//!
//! let cb_desc: &[ShortCodebookDesc] = &[
//!             ShortCodebookDesc { code: 0b00,   bits: 2 },
//!             ShortCodebookDesc { code: 0,      bits: 0 },
//!             ShortCodebookDesc { code: 0b01,   bits: 2 },
//!             ShortCodebookDesc { code: 0b1,    bits: 1 }];
//! let mut cr = ShortCodebookDescReader::new(cb_desc);
//! let cb = Codebook::new(&mut cr, CodebookMode::MSB).unwrap();
//! ```
//!
//! Create a codebook using more flexible [`TableCodebookDescReader`] approach.
//! This will create a codebook for the following set: `1` -> -2, `01` -> -1, `001` -> 0, `0001` -> 1, `00001` -> 2.
//! ```
//! use nihav_core::io::codebook::{TableCodebookDescReader, Codebook, CodebookMode};
//!
//! fn map_cb_index(index: usize) -> i16 { (index as i16) - 2 }
//! const CB_BITS:  [u8; 5] = [ 1, 2, 3, 4, 5 ];
//! const CB_CODES: [u8; 5] = [ 1, 1, 1, 1, 1 ];
//!
//! let mut tcr = TableCodebookDescReader::new(&CB_CODES, &CB_BITS, map_cb_index);
//! let cb = Codebook::new(&mut tcr, CodebookMode::MSB).unwrap();
//! ```
//!
//! Read value using a codebook:
//! ```no_run
//! use nihav_core::io::bitreader::BitReader;
//! use nihav_core::io::codebook::{Codebook, CodebookReader, CodebookMode};
//! # use nihav_core::io::codebook::{ShortCodebookDesc, ShortCodebookDescReader, CodebookDescReader, CodebookResult};
//!
//! # fn foo(br: &mut BitReader) -> CodebookResult<()> {
//! # let mut cr = ShortCodebookDescReader::new(&[ShortCodebookDesc { code: 0b00,   bits: 2 }]);
//! let cb = Codebook::new(&mut cr, CodebookMode::MSB).unwrap();
//! let value = br.read_cb(&cb)?;
//! # Ok(())
//! # }
//! ```
//!
//! [`MSB`]: ./enum.CodebookMode.html#variant.MSB
//! [`LSB`]: ./enum.CodebookMode.html#variant.LSB
//! [`CodebookDescReader`]: ./trait.CodebookDescReader.html
//! [`TableCodebookDescReader`]: ./struct.TableCodebookDescReader.html

use std::collections::HashMap;
use std::cmp::{max, min};
use super::bitreader::BitReader;

/// A list specifying general codebook operations errors.
#[derive(Debug)]
pub enum CodebookError {
    /// Codebook description contains errors.
    InvalidCodebook,
    /// Could not allocate memory for codebook.
    MemoryError,
    /// Bitstream contains a sequence not present in codebook.
    InvalidCode,
}

/// Codebook operation modes.
#[derive(Debug, Copy, Clone)]
pub enum CodebookMode {
    /// Codes in the codebook should be read most significant bit first.
    MSB,
    /// Codes in the codebook should be read least significant bit first.
    LSB,
}

/// A specialised `Result` type for codebook operations.
pub type CodebookResult<T> = Result<T, CodebookError>;

/// Codebook description for `(code bits, code length, code value)` triplet.
///
/// This should be used to create a list of codeword definitions for [`FullCodebookDescReader`].
///
/// [`FullCodebookDescReader`]: ./struct.FullCodebookDescReader.html
#[derive(Clone,Copy)]
pub struct FullCodebookDesc<S> {
    /// Codeword bits.
    pub code: u32,
    /// Codeword length.
    pub bits: u8,
    /// Codeword value (symbol).
    pub sym:  S,
}

/// Codebook description for `(code bits, code length)` pair with array index being used as codeword value.
///
/// This should be used to create a list of codeword definitions for [`ShortCodebookDescReader`].
///
/// [`ShortCodebookDescReader`]: ./struct.ShortCodebookDescReader.html
#[derive(Clone,Copy)]
pub struct ShortCodebookDesc {
    /// Codeword bits.
    pub code: u32,
    /// Codeword length.
    pub bits: u8,
}

/// The interface for providing a list of codeword definitions to the codebook creator.
///
/// The structure implementing this trait should be able to provide the total number of defined codewords and their bits and values. [`ShortCodebookDescReader`] or [`TableCodebookDescReader`] are some examples of such implementation.
/// Codeword definitions with zero length are ignored (those may be used to create sparse codebook definitions though).
///
/// [`ShortCodebookDescReader`]: ./struct.ShortCodebookDescReader.html
/// [`TableCodebookDescReader`]: ./struct.TableCodebookDescReader.html
#[allow(clippy::len_without_is_empty)]
pub trait CodebookDescReader<S> {
    /// Returns the codeword length for the provided index.
    fn bits(&mut self, idx: usize) -> u8;
    /// Returns the codeword bits for the provided index.
    fn code(&mut self, idx: usize) -> u32;
    /// Returns the codeword value (aka codeword symbol) for the provided index.
    fn sym (&mut self, idx: usize) -> S;
    /// Returns the total number of defined codewords.
    fn len (&mut self)             -> usize;
}

/// The codebook structure for code reading.
#[allow(dead_code)]
pub struct Codebook<S> {
    pub table: Vec<u32>,
    pub syms:  Vec<S>,
    pub lut_bits: u8,
}

/// Trait allowing bitreader to use codebook for decoding bit sequences.
pub trait CodebookReader<S> {
    /// Reads the codeword from the bitstream and returns its value (or [`InvalidCode`] on error).
    ///
    /// [`InvalidCode`]: ./enum.CodebookError.html#variant.InvalidCode
    fn read_cb(&mut self, cb: &Codebook<S>) -> CodebookResult<S>;
}

pub const TABLE_FILL_VALUE: u32 = 0x7F;
const MAX_LUT_BITS: u8 = 10;

fn fill_lut_msb(table: &mut [u32], off: usize,
                code: u32, bits: u8, lut_bits: u8, symidx: u32, esc: bool) -> CodebookResult<()> {
    if !esc {
        let fill_len  = lut_bits - bits;
        let fill_size = 1 << fill_len;
        let fill_code = code << (lut_bits - bits);
        let lut_value = (symidx << 8) | u32::from(bits);
        for j in 0..fill_size {
            let idx = (fill_code + j) as usize;
            if table[idx + off] != TABLE_FILL_VALUE { return Err(CodebookError::InvalidCodebook); }
            table[idx + off] = lut_value;
        }
    } else {
        let idx = (code as usize) + off;
        if table[idx] != TABLE_FILL_VALUE { return Err(CodebookError::InvalidCodebook); }
        table[idx] = (symidx << 8) | 0x80 | u32::from(bits);
    }
    Ok(())
}

fn fill_lut_lsb(table: &mut [u32], off: usize,
                code: u32, bits: u8, lut_bits: u8, symidx: u32, esc: bool) -> CodebookResult<()> {
    if !esc {
        let fill_len  = lut_bits - bits;
        let fill_size = 1 << fill_len;
        let fill_code = code;
        let step = lut_bits - fill_len;
        for j in 0..fill_size {
            let idx = (fill_code + (j << step)) as usize;
            if table[idx + off] != TABLE_FILL_VALUE { return Err(CodebookError::InvalidCodebook); }
            table[idx + off] = (symidx << 8) | u32::from(bits);
        }
    } else {
        let idx = (code as usize) + off;
        if table[idx] != TABLE_FILL_VALUE { return Err(CodebookError::InvalidCodebook); }
        table[idx] = (symidx << 8) | 0x80 | u32::from(bits);
    }
    Ok(())
}

fn fill_lut(table: &mut [u32], mode: CodebookMode,
            off: usize, code: u32, bits: u8, lut_bits: u8, symidx: u32, esc: bool) -> CodebookResult<bool> {
    match mode {
        CodebookMode::MSB => fill_lut_msb(table, off, code, bits, lut_bits, symidx, esc)?,
        CodebookMode::LSB => fill_lut_lsb(table, off, code, bits, lut_bits, symidx, esc)?,
    };
    Ok(bits > lut_bits)
}

fn resize_table(table: &mut Vec<u32>, bits: u8) -> CodebookResult<u32> {
    let add_size = (1 << bits) as usize;
    table.reserve(add_size);
    let cur_off = table.len() as u32;
    let new_size = table.len() + add_size;
    if table.capacity() < new_size { return Err(CodebookError::MemoryError); }
    table.resize(new_size, TABLE_FILL_VALUE);
    Ok(cur_off)
}


fn extract_lut_part(code: u32, bits: u8, lut_bits: u8, mode: CodebookMode) -> u32 {
    match mode {
        CodebookMode::MSB => code >> (bits - lut_bits),
        CodebookMode::LSB => code & ((1 << lut_bits) - 1),
    }
}

fn extract_esc_part(code: u32, bits: u8, lut_bits: u8, mode: CodebookMode) -> u32 {
    match mode {
        CodebookMode::MSB => code & ((1 << (bits - lut_bits)) - 1),
        CodebookMode::LSB => code >> lut_bits,
    }
}

#[derive(Clone,Copy)]
struct Code {
    code: u32,
    bits: u8,
    idx:  usize,
}

struct CodeBucket {
    maxlen: u8,
    offset: usize,
    codes:  Vec<Code>,
}

impl CodeBucket {
    fn new() -> Self {
        CodeBucket { maxlen: 0, offset: 0, codes: Vec::new() }
    }
    fn add_code(&mut self, c: Code) {
        if c.bits > self.maxlen { self.maxlen = c.bits; }
        self.codes.push(c);
    }
}

type EscapeCodes = HashMap<u32, CodeBucket>;

fn add_esc_code(cc: &mut EscapeCodes, key: u32, code: u32, bits: u8, idx: usize) {
    cc.entry(key).or_insert_with(CodeBucket::new);
    let b = cc.get_mut(&key);
    if let Some(bucket) = b {
        bucket.add_code(Code {code, bits, idx });
    } else { panic!("no bucket when expected!"); }
}

fn build_esc_lut(table: &mut Vec<u32>,
                 mode: CodebookMode,
                 bucket: &CodeBucket) -> CodebookResult<()> {
    let mut escape_list: EscapeCodes = HashMap::new();
    let maxlen = if bucket.maxlen > MAX_LUT_BITS { MAX_LUT_BITS } else { bucket.maxlen };

    for code in &bucket.codes {
        let bits = code.bits;
        if code.bits <= MAX_LUT_BITS {
            fill_lut(table, mode, bucket.offset, code.code, bits,
                     maxlen, code.idx as u32, false)?;
        } else {
            let ckey = extract_lut_part(code.code, bits, MAX_LUT_BITS, mode);
            let cval = extract_esc_part(code.code, bits, MAX_LUT_BITS, mode);
            add_esc_code(&mut escape_list, ckey, cval, bits - MAX_LUT_BITS, code.idx);
        }
    }

    let cur_offset = bucket.offset;
    for (ckey, sec_bucket) in &mut escape_list {
        let key = *ckey;
        let maxlen = min(sec_bucket.maxlen, MAX_LUT_BITS);
        let new_off = resize_table(table, maxlen)?;
        fill_lut(table, mode, cur_offset, key, maxlen,
                 MAX_LUT_BITS, new_off, true)?;
        sec_bucket.offset = new_off as usize;
    }

    for sec_bucket in escape_list.values() {
        build_esc_lut(table, mode, sec_bucket)?;
    }

    Ok(())
}

impl<S: Copy> Codebook<S> {

    /// Constructs a new `Codebook` instance using provided codebook description and mode.
    pub fn new(cb: &mut dyn CodebookDescReader<S>, mode: CodebookMode) -> CodebookResult<Self> {
        let mut maxbits = 0;
        let mut nnz = 0;
        let mut escape_list: EscapeCodes = HashMap::new();

        let mut symidx: usize = 0;
        for i in 0..cb.len() {
            let bits = cb.bits(i);
            if bits > 0 {
                nnz += 1;
                if cb.code(i) >= (1 << bits) {
                    return Err(CodebookError::InvalidCodebook);
                }
            }
            maxbits = max(bits, maxbits);
            if bits > MAX_LUT_BITS {
                let code = cb.code(i);
                let ckey = extract_lut_part(code, bits, MAX_LUT_BITS, mode);
                let cval = extract_esc_part(code, bits, MAX_LUT_BITS, mode);
                add_esc_code(&mut escape_list, ckey, cval, bits - MAX_LUT_BITS, symidx);
            }
            if bits > 0 { symidx += 1; }
        }
        if maxbits == 0 { return Err(CodebookError::InvalidCodebook); }

        if maxbits > MAX_LUT_BITS { maxbits = MAX_LUT_BITS; }

        let tab_len = 1 << maxbits;
        let mut table: Vec<u32> = Vec::with_capacity(tab_len);
        let mut syms:  Vec<S>   = Vec::with_capacity(nnz);
        if table.capacity() < tab_len { return Err(CodebookError::MemoryError); }
        if syms.capacity()  < nnz     { return Err(CodebookError::MemoryError); }
        table.resize(tab_len, TABLE_FILL_VALUE);

        let mut symidx: u32 = 0;
        for i in 0..cb.len() {
            let bits = cb.bits(i);
            let code = cb.code(i);
            if bits == 0 { continue; }
            if bits <= MAX_LUT_BITS {
                fill_lut(&mut table, mode, 0, code, bits, maxbits, symidx, false)?;
            } else {
                let ckey = extract_lut_part(code, bits, MAX_LUT_BITS, mode) as usize;
                if table[ckey] == TABLE_FILL_VALUE {
                    let key = ckey as u32;
                    if let Some(bucket) = escape_list.get_mut(&key) {
                        let maxlen = min(bucket.maxlen, MAX_LUT_BITS);
                        let new_off = resize_table(&mut table, maxlen)?;
                        fill_lut(&mut table, mode, 0, key, maxlen, MAX_LUT_BITS, new_off, true)?;
                        bucket.offset = new_off as usize;
                    }
                }
            }
            symidx += 1;
        }

        for bucket in escape_list.values() {
            build_esc_lut(&mut table, mode, bucket)?;
        }

        for i in 0..cb.len() {
            if cb.bits(i) > 0 {
                syms.push(cb.sym(i));
            }
        }

        Ok(Codebook { table, syms, lut_bits: maxbits })
    }
}

impl<'a, S: Copy> CodebookReader<S> for BitReader<'a> {
    fn read_cb(&mut self, cb: &Codebook<S>) -> CodebookResult<S> {
        let mut esc = true;
        let mut idx = 0;
        let mut lut_bits = cb.lut_bits;
        while esc {
            let lut_idx = (self.peek(lut_bits) as usize) + idx;
            if cb.table[lut_idx] == TABLE_FILL_VALUE { return Err(CodebookError::InvalidCode); }
            let bits = cb.table[lut_idx] & 0x7F;
            esc  = (cb.table[lut_idx] & 0x80) != 0;
            idx  = (cb.table[lut_idx] >> 8) as usize;
            let skip_bits = if esc { u32::from(lut_bits) } else { bits };
            if (skip_bits as isize) > self.left() {
                return Err(CodebookError::InvalidCode);
            }
            self.skip(skip_bits).unwrap();
            lut_bits = bits as u8;
        }
        Ok(cb.syms[idx])
    }
}

/// Codebook description that stores a reference to the list of codewords and their values.
pub struct FullCodebookDescReader<'a, S> {
    data: &'a [FullCodebookDesc<S>],
}

impl<'a, S> FullCodebookDescReader<'a, S> {
    /// Constructs a new `FullCodebookDescReader` instance.
    pub fn new(data: &'a [FullCodebookDesc<S>]) -> Self {
        FullCodebookDescReader { data }
    }
}

impl<'a, S: Copy> CodebookDescReader<S> for FullCodebookDescReader<'a, S> {
    fn bits(&mut self, idx: usize) -> u8  { self.data[idx].bits }
    fn code(&mut self, idx: usize) -> u32 { self.data[idx].code }
    fn sym (&mut self, idx: usize) -> S   { self.data[idx].sym  }
    fn len(&mut self) -> usize { self.data.len() }
}

/// Codebook description that stores a reference to the list of codewords (their value is equal to the index).
pub struct ShortCodebookDescReader<'a> {
    data: &'a [ShortCodebookDesc],
}

impl<'a> ShortCodebookDescReader<'a> {
    /// Constructs a new `ShortCodebookDescReader` instance.
    pub fn new(data: &'a [ShortCodebookDesc]) -> Self {
        ShortCodebookDescReader { data }
    }
}

impl<'a> CodebookDescReader<u32> for ShortCodebookDescReader<'a> {
    fn bits(&mut self, idx: usize) -> u8  { self.data[idx].bits }
    fn code(&mut self, idx: usize) -> u32 { self.data[idx].code }
    fn sym (&mut self, idx: usize) -> u32 { idx as u32 }
    fn len(&mut self) -> usize { self.data.len() }
}

/// Flexible codebook description that uses two separate arrays for codeword bits and lengths and a function that maps codeword index into its symbol.
pub struct TableCodebookDescReader<'a, CodeType:'static, IndexType:'static> {
    bits:       &'a [u8],
    codes:      &'a [CodeType],
    idx_map:    Box<dyn Fn(usize) -> IndexType>,
}

impl<'a, CodeType, IndexType> TableCodebookDescReader<'a, CodeType, IndexType> {
    /// Constructs a new `TableCodebookDescReader` instance.
    pub fn new<MapFunc: Fn(usize) -> IndexType + 'static>(codes: &'a [CodeType], bits: &'a [u8], map_func: MapFunc) -> Self {
        Self { bits, codes, idx_map: Box::new(map_func) }
    }
}
impl<'a, CodeType: Copy+Into<u32>, IndexType> CodebookDescReader<IndexType> for TableCodebookDescReader<'a, CodeType, IndexType>
{
    fn bits(&mut self, idx: usize) -> u8  { self.bits[idx] }
    fn code(&mut self, idx: usize) -> u32 { self.codes[idx].into() }
    fn sym (&mut self, idx: usize) -> IndexType { (self.idx_map)(idx) }
    fn len(&mut self) -> usize { self.bits.len() }
}

#[cfg(test)]
mod test {
    use super::*;
    use crate::io::bitreader::*;

    #[test]
    fn test_cb() {
        const BITS: [u8; 2] = [0b01011011, 0b10111100];
        let cb_desc = &[
            FullCodebookDesc { code: 0b0,    bits: 1, sym:  16 },
            FullCodebookDesc { code: 0b10,   bits: 2, sym:  -3 },
            FullCodebookDesc { code: 0b110,  bits: 3, sym:  42 },
            FullCodebookDesc { code: 0b1110, bits: 4, sym: -42 }
        ];
        let buf = &BITS;
        let mut br = BitReader::new(buf, BitReaderMode::BE);
        let mut cfr = FullCodebookDescReader::new(cb_desc);
        let cb = Codebook::new(&mut cfr, CodebookMode::MSB).unwrap();
        assert_eq!(br.read_cb(&cb).unwrap(),  16);
        assert_eq!(br.read_cb(&cb).unwrap(),  -3);
        assert_eq!(br.read_cb(&cb).unwrap(),  42);
        assert_eq!(br.read_cb(&cb).unwrap(), -42);
        let ret = br.read_cb(&cb);
        if let Err(e) = ret {
            assert_eq!(e as i32, CodebookError::InvalidCode as i32);
        } else {
            assert_eq!(0, 1);
        }

        let scb_desc = &[
            ShortCodebookDesc { code: 0b0,    bits: 1 },
            ShortCodebookDesc { code: 0,      bits: 0 },
            ShortCodebookDesc { code: 0b10,   bits: 2 },
            ShortCodebookDesc { code: 0,      bits: 0 },
            ShortCodebookDesc { code: 0,      bits: 0 },
            ShortCodebookDesc { code: 0b110,  bits: 3 },
            ShortCodebookDesc { code: 0,      bits: 0 },
            ShortCodebookDesc { code: 0b11100, bits: 5 },
            ShortCodebookDesc { code: 0b11101, bits: 5 },
            ShortCodebookDesc { code: 0b1111010, bits: 7 },
            ShortCodebookDesc { code: 0b1111011, bits: 7 },
            ShortCodebookDesc { code: 0b1111110, bits: 7 },
            ShortCodebookDesc { code: 0b11111111, bits: 8 }
        ];
        let mut br2 = BitReader::new(buf, BitReaderMode::BE);
        let mut cfr = ShortCodebookDescReader::new(scb_desc);
        let cb = Codebook::new(&mut cfr, CodebookMode::MSB).unwrap();
        assert_eq!(br2.read_cb(&cb).unwrap(), 0);
        assert_eq!(br2.read_cb(&cb).unwrap(), 2);
        assert_eq!(br2.read_cb(&cb).unwrap(), 5);
        assert_eq!(br2.read_cb(&cb).unwrap(), 8);

        assert_eq!(reverse_bits(0b0000_0101_1011_1011_1101_1111_0111_1111, 32),
                                0b1111_1110_1111_1011_1101_1101_1010_0000);

        const BITS_LE: [u8; 3] = [0b11101111, 0b01110010, 0b01];
        let buf = &BITS_LE;
        let scble_desc = &[
            ShortCodebookDesc { code: 0b00,   bits: 2 },
            ShortCodebookDesc { code: 0,      bits: 0 },
            ShortCodebookDesc { code: 0b01,   bits: 2 },
            ShortCodebookDesc { code: 0,      bits: 0 },
            ShortCodebookDesc { code: 0,      bits: 0 },
            ShortCodebookDesc { code: 0b011,  bits: 3 },
            ShortCodebookDesc { code: 0,      bits: 0 },
            ShortCodebookDesc { code: 0b10111, bits: 5 },
            ShortCodebookDesc { code: 0b00111, bits: 5 },
            ShortCodebookDesc { code: 0b0101111, bits: 7 },
            ShortCodebookDesc { code: 0b0111111, bits: 7 },
            ShortCodebookDesc { code: 0b1011101111, bits: 10 }
        ];
        let mut brl = BitReader::new(buf, BitReaderMode::LE);
        let mut cfr = ShortCodebookDescReader::new(scble_desc);
        let cb = Codebook::new(&mut cfr, CodebookMode::LSB).unwrap();
        assert_eq!(brl.read_cb(&cb).unwrap(), 11);
        assert_eq!(brl.read_cb(&cb).unwrap(), 0);
        assert_eq!(brl.read_cb(&cb).unwrap(), 7);
        assert_eq!(brl.read_cb(&cb).unwrap(), 0);
    }
}
