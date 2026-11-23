//! Some universal integer codes support for bitstream reader.
use crate::io::bitreader::{BitReader, BitReaderError, BitReaderResult};
use crate::io::bitwriter::BitWriter;

/// Unsigned integer code types.
#[derive(Debug)]
pub enum UintCodeType {
    /// Code where number is represented as run of ones with terminating zero.
    UnaryOnes,
    /// Code where number is represented as run of zeroes with terminating one.
    UnaryZeroes,
    /// Code for 0, 1 and 2 coded as `0`, `10` and `11`
    Unary012,
    /// Code for 0, 1 and 2 coded as `11`, `10` and `0`
    Unary210,
    /// General limited unary code with defined run and terminating bit.
    LimitedUnary(u32, u32),
    /// Limited run of zeroes with terminating one (unless the code has maximum length).
    ///
    /// [`Unary012`] is essentially an alias for `LimitedZeroes(2)`.
    ///
    /// [`Unary012`]: #variant.Unary012
    LimitedZeroes(u32),
    /// Limited run of one with terminating zero (unless the code has maximum length).
    LimitedOnes(u32),
    /// Golomb code.
    Golomb(u8),
    /// Rice code.
    Rice(u8),
    /// Elias Gamma code (interleaved).
    Gamma,
    /// Elias Gamma' code (sometimes incorrectly called exp-Golomb).
    GammaP,
}

/// Signed integer code types.
pub enum IntCodeType {
    /// Golomb code. Last bit represents the sign.
    Golomb(u8),
    /// Golomb code. Last bit represents the sign.
    Rice(u8),
    /// Elias Gamma code. Unsigned values are remapped as 0, 1, -1, 2, -2, ...
    Gamma,
    /// Elias Gamma' code. Unsigned values are remapped as 0, 1, -1, 2, -2, ...
    GammaP,
}

/// Universal integer code reader trait for bitstream reader.
///
/// # Examples
///
/// Read an unsigned Golomb code:
/// ````
/// use nihav_core::io::bitreader::*;
/// use nihav_core::io::intcode::{IntCodeReader,UintCodeType};
///
/// # fn foo() -> BitReaderResult<()> {
/// let mem: [u8; 4] = [ 0, 1, 2, 3];
/// let mut br = BitReader::new(&mem, BitReaderMode::BE);
/// let val = br.read_code(UintCodeType::Golomb(3))?;
/// # Ok(())
/// # }
/// ````
///
/// Read signed Elias code:
/// ````
/// use nihav_core::io::bitreader::*;
/// use nihav_core::io::intcode::{IntCodeReader,IntCodeType};
///
/// # fn foo() -> BitReaderResult<()> {
/// let mem: [u8; 4] = [ 0, 1, 2, 3];
/// let mut br = BitReader::new(&mem, BitReaderMode::BE);
/// let val = br.read_code_signed(IntCodeType::Gamma)?;
/// # Ok(())
/// # }
/// ````
pub trait IntCodeReader {
    /// Reads an unsigned integer code of requested type.
    fn read_code(&mut self, t: UintCodeType) -> BitReaderResult<u32>;
    /// Reads signed integer code of requested type.
    fn read_code_signed(&mut self, t: IntCodeType) -> BitReaderResult<i32>;
}

fn read_unary(br: &mut BitReader, terminator: u32) -> BitReaderResult<u32> {
    let mut res: u32 = 0;
    loop {
        if br.read(1)? == terminator { return Ok(res); }
        res += 1;
    }
}

fn read_unary_lim(br: &mut BitReader, len: u32, terminator: u32) -> BitReaderResult<u32> {
    let mut res: u32 = 0;
    loop {
        if br.read(1)? == terminator { return Ok(res); }
        res += 1;
        if res == len { return Ok(res); }
    }
}

fn read_unary210(br: &mut BitReader) -> BitReaderResult<u32> {
    let val = read_unary_lim(br, 2, 0)?;
    Ok(2 - val)
}

fn read_golomb(br: &mut BitReader, m: u8) -> BitReaderResult<u32> {
    if m == 0 { return Err(BitReaderError::InvalidValue); }
    let nbits = (8 - m.leading_zeros()) as u8;
    if (m & (m - 1)) == 0 { return read_rice(br, nbits); }
    let cutoff = u32::from((1 << nbits) - m);
    let pfx = read_unary(br, 0)?;
    let tail = br.read(nbits - 1)?;
    if tail < cutoff {
        let res = pfx * u32::from(m) + tail;
        Ok (res)
    } else {
        let add = br.read(1)?;
        let res = pfx * u32::from(m) + (tail - cutoff) * 2 + add + cutoff;
        Ok (res)
    }
}

fn read_rice(br: &mut BitReader, k: u8) -> BitReaderResult<u32> {
    let pfx = read_unary(br, 1)?;
    let ret = (pfx << k) + br.read(k)?;
    Ok(ret)
}

fn read_gamma(br: &mut BitReader) -> BitReaderResult<u32> {
    let mut ret = 1;
    while br.read(1)? != 1 {
        ret = (ret << 1) | br.read(1)?;
    }
    Ok(ret - 1)
}

fn read_gammap(br: &mut BitReader) -> BitReaderResult<u32> {
    let pfx = read_unary(br, 1)?;
    if pfx > 32 { return Err(BitReaderError::InvalidValue); }
    let ret = (1 << pfx) + br.read(pfx as u8)?;
    Ok(ret)
}

fn uval_to_sval0mp(uval: u32) -> i32 {
    if (uval & 1) != 0 { -(((uval + 1) >> 1) as i32) }
    else               { (uval >> 1) as i32 }
}

fn uval_to_sval0pm(uval: u32) -> i32 {
    if (uval & 1) != 0 { ((uval + 1) >> 1) as i32 }
    else               { -((uval >> 1) as i32) }
}

impl<'a> IntCodeReader for BitReader<'a> {
    #[inline(always)]
    fn read_code(&mut self, t: UintCodeType) -> BitReaderResult<u32> {
        match t {
            UintCodeType::UnaryOnes               => read_unary(self, 0),
            UintCodeType::UnaryZeroes             => read_unary(self, 1),
            UintCodeType::LimitedZeroes(len)      => read_unary_lim(self, len, 1),
            UintCodeType::LimitedOnes(len)        => read_unary_lim(self, len, 0),
            UintCodeType::LimitedUnary(len, term) => read_unary_lim(self, len, term),
            UintCodeType::Unary012                => read_unary_lim(self, 2, 0),
            UintCodeType::Unary210                => read_unary210(self),
            UintCodeType::Golomb(m)               => read_golomb(self, m),
            UintCodeType::Rice(k)                 => read_rice(self, k),
            UintCodeType::Gamma                   => read_gamma(self),
            UintCodeType::GammaP                  => read_gammap(self),
        }
    }
    fn read_code_signed(&mut self, t: IntCodeType) -> BitReaderResult<i32> {
        let uval =
            match t {
                IntCodeType::Golomb(m)               => read_golomb(self, m)?,
                IntCodeType::Rice(k)                 => read_rice(self, k)?,
                IntCodeType::Gamma                   => read_gamma(self)?,
                IntCodeType::GammaP                  => read_gammap(self)?,
            };
        match t {
            IntCodeType::Golomb(_)               => Ok(uval_to_sval0mp(uval)),
            IntCodeType::Rice(_)                 => Ok(uval_to_sval0mp(uval)),
            IntCodeType::Gamma                   => Ok(uval_to_sval0pm(uval)),
            IntCodeType::GammaP                  => Ok(uval_to_sval0pm(uval)),
        }
    }
}

/// Universal integer code writer trait for bitstream writer.
///
/// # Examples
///
/// Write an unsigned Golomb code:
/// ````
/// use nihav_core::io::bitwriter::*;
/// use nihav_core::io::intcode::{IntCodeWriter,UintCodeType};
///
/// let mut bw = BitWriter::new(Vec::new(), BitWriterMode::BE);
/// bw.write_code(UintCodeType::Golomb(3), 42);
/// ````
///
/// Write signed Elias code:
/// ````
/// use nihav_core::io::bitwriter::*;
/// use nihav_core::io::intcode::{IntCodeWriter,IntCodeType};
///
/// let mut bw = BitWriter::new(Vec::new(), BitWriterMode::BE);
/// bw.write_code_signed(IntCodeType::Gamma, 42);
/// ````
pub trait IntCodeWriter {
    /// Writes an unsigned integer code of requested type.
    fn write_code(&mut self, t: UintCodeType, val: u32);
    /// Writes signed integer code of requested type.
    fn write_code_signed(&mut self, t: IntCodeType, val: i32);
}

impl IntCodeWriter for BitWriter {
    #[inline(always)]
    fn write_code(&mut self, t: UintCodeType, val: u32) {
        match t {
            UintCodeType::UnaryOnes               => write_unary(self, val, 0),
            UintCodeType::UnaryZeroes             => write_unary(self, val, 1),
            UintCodeType::LimitedZeroes(len)      => write_unary_lim(self, val, len, 1),
            UintCodeType::LimitedOnes(len)        => write_unary_lim(self, val, len, 0),
            UintCodeType::LimitedUnary(len, term) => write_unary_lim(self, val, len, term),
            UintCodeType::Unary012                => write_unary_lim(self, val, 2, 0),
            UintCodeType::Unary210                => write_unary210(self, val),
            UintCodeType::Golomb(m)               => write_golomb(self, val, m),
            UintCodeType::Rice(k)                 => write_rice(self, val, k),
            UintCodeType::Gamma                   => write_gamma(self, val),
            UintCodeType::GammaP                  => write_gammap(self, val),
        };
    }
    fn write_code_signed(&mut self, t: IntCodeType, val: i32) {
        match t {
            IntCodeType::Golomb(m)  => write_golomb(self, sval0mp_to_uval(val), m),
            IntCodeType::Rice(k)    => write_rice(self, sval0mp_to_uval(val), k),
            IntCodeType::Gamma      => write_gamma(self, sval0pm_to_uval(val)),
            IntCodeType::GammaP     => write_gammap(self, sval0pm_to_uval(val)),
        };
    }
}

fn sval0mp_to_uval(val: i32) -> u32 {
    if val < 0  { (-val as u32) * 2 - 1 }
    else        { (val as u32) * 2 }
}

fn sval0pm_to_uval(val: i32) -> u32 {
    if val >= 0 { (val as u32) * 2 + 1 }
    else        { (-val as u32) * 2 }
}

fn write_unary210(bw: &mut BitWriter, val: u32) {
    bw.write_bit(val == 0);
    if val != 0 {
        bw.write_bit(val == 1);
    }
}

fn write_unary(bw: &mut BitWriter, val: u32, term: u32) {
    let term = term != 0;
    for _ in 0..val {
        bw.write_bit(!term);
    }
    bw.write_bit(term);
}

fn write_unary_lim(bw: &mut BitWriter, val: u32, maxval: u32, term: u32) {
    let term = term != 0;
    for _ in 0..val {
        bw.write_bit(!term);
    }
    if val < maxval {
        bw.write_bit(term);
    }
}

fn write_rice(bw: &mut BitWriter, val: u32, k: u8) {
    let mut exp = val >> k;
    while exp >= 16 {
        bw.write(0, 16);
        exp -= 16
    }
    if exp > 0 {
        bw.write(0, exp as u8);
    }
    bw.write1();
    if k > 0 {
        let mant = val & ((1 << k) - 1);
        bw.write(mant, k);
    }
}

fn write_golomb(bw: &mut BitWriter, val: u32, m: u8) {
    if m == 0 { return; }
    let nbits = (8 - m.leading_zeros()) as u8;
    if (m & (m - 1)) == 0 { return write_rice(bw, val, nbits); }
    let q = val / u32::from(m);
    let r = val % u32::from(m);
    let cutoff = u32::from((1 << nbits) - m);

    write_unary(bw, q, 0);
    if r < cutoff {
        bw.write(r, nbits - 1);
    } else {
        bw.write(r + cutoff, nbits);
    }
}

fn write_gamma(bw: &mut BitWriter, val: u32) {
    let val = val + 1;
    let bits = (32 - val.leading_zeros()) as u8;
    let mut mask = 1 << bits >> 2;
    while mask != 0 {
        bw.write0();
        bw.write_bit((val & mask) != 0);
        mask >>= 1;
    }
    bw.write1();
}

fn write_gammap(bw: &mut BitWriter, val: u32) {
    let bits = 31 - val.leading_zeros();
    write_unary(bw, bits, 1);
    bw.write(val - (1 << bits), bits as u8);
}

#[cfg(test)]
mod test {
    use super::*;
    use crate::io::bitreader::*;
    use crate::io::bitwriter::*;

    #[test]
    fn int_codes() {
        const GDATA: [u8; 6] = [0b000_001_01, 0b0_0110_011, 0b1_1000_100, 0b1_1010_101, 0b10_10111_1, 0b1000_0000];
        let src = &GDATA;
        let mut br = BitReader::new(src, BitReaderMode::BE);
        for i in 0..11 {
            assert_eq!(br.read_code(UintCodeType::Golomb(5)).unwrap(), i);
        }
    }
    #[test]
    fn rw_codes() {
        let mut bw = BitWriter::new(Vec::new(), BitWriterMode::BE);
        bw.write_code(UintCodeType::Golomb(5), 42);
        bw.write_code(UintCodeType::Gamma, 42);
        bw.write_code(UintCodeType::GammaP, 42);
        let data = bw.end();

        let mut br = BitReader::new(&data, BitReaderMode::BE);
        assert_eq!(br.read_code(UintCodeType::Golomb(5)).unwrap(), 42);
        assert_eq!(br.read_code(UintCodeType::Gamma).unwrap(), 42);
        assert_eq!(br.read_code(UintCodeType::GammaP).unwrap(), 42);
    }
}
