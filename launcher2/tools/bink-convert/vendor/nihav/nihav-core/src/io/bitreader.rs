//! Bitstream reader functionality.
//!
//! Bitstream reader operates on `&[u8]` and allows to read bits from the slice in different modes.
//!
//! # Examples
//!
//! Reading 17 bits from a bitstream:
//! ```
//! use nihav_core::io::bitreader::{BitReader,BitReaderMode};
//!
//! # use nihav_core::io::bitreader::BitReaderResult;
//! # fn foo() -> BitReaderResult<u32> {
//! let bits: [u8; 4] = [ 42, 43, 44, 45 ];
//! let mut br = BitReader::new(&bits, BitReaderMode::BE);
//! let value = br.read(17)?;
//! # Ok(value)
//! # }
//! ```
//!
//! Reading some amount of bits and checking how many bits are left:
//! ```
//! use nihav_core::io::bitreader::{BitReader,BitReaderMode};
//!
//! # use nihav_core::io::bitreader::BitReaderResult;
//! # fn foo() -> BitReaderResult<()> {
//! let bits: [u8; 4] = [ 42, 43, 44, 45 ];
//! let mut br = BitReader::new(&bits, BitReaderMode::BE);
//! let num_skip_bits = br.read(3)?;
//! br.skip(num_skip_bits)?;
//! println!("Now there are {} bits left to read.", br.left());
//! # Ok(())
//! # }
//! ```



/// Bitstream reading modes.
#[derive(Debug,Clone,Copy)]
pub enum BitReaderMode {
    /// The stream is big endian MSB first.
    BE,
    /// The stream is little endian LSB first.
    LE,
    /// The stream is packed into 16-bit little-endian words MSB first.
    LE16MSB,
    /// The stream is packed into 32-bit little-endian words MSB first.
    LE32MSB,
}

/// A list specifying general bitstream reading errors.
#[derive(Debug,Clone,Copy)]
pub enum BitReaderError {
    /// The reader is at the end of bitstream.
    BitstreamEnd,
    /// The caller tried to read too many bits at once (e.g. 128).
    TooManyBitsRequested,
    /// Some argument is invalid.
    InvalidValue,
}

use self::BitReaderError::*;

/// A specialised `Result` type for bitstream operations.
pub type BitReaderResult<T> = Result<T, BitReaderError>;

/// Bitstream reader.
#[derive(Debug,Clone)]
pub struct BitReader<'a> {
    cache: u64,
    bits:  u8,
    pos:   usize,
    src:   &'a [u8],
    mode:  BitReaderMode,
}

#[allow(clippy::identity_op)]
impl<'a> BitReader<'a> {

    /// Constructs a new instance of bitstream reader.
    ///
    /// # Examples
    ///
    /// ```
    /// use nihav_core::io::bitreader::{BitReader,BitReaderMode};
    ///
    /// let bits: [u8; 4] = [ 42, 43, 44, 45 ];
    /// let mut br = BitReader::new(&bits, BitReaderMode::BE);
    /// ```
    pub fn new(src: &'a [u8], mode: BitReaderMode) -> Self {
        BitReader{ cache: 0, pos: 0, bits: 0, src, mode }
    }

    /// Returns the data bitstream reader uses.
    pub fn get_data(&self) -> &'a [u8] { self.src }

    /// Reports the current bit position in the bitstream (usually simply the number of bits read so far).
    pub fn tell(&self) -> usize {
        self.pos * 8 - (self.bits as usize)
    }

    /// Reports the amount of bits left until the end of the bitstream.
    pub fn left(&self) -> isize {
        ((self.src.len() as isize) - (self.pos as isize)) * 8 + (self.bits as isize)
    }

    fn fill32be(&mut self, src: &[u8]) {
        let nw = (u32::from(src[0]) << 24) |
                 (u32::from(src[1]) << 16) |
                 (u32::from(src[2]) <<  8) |
                 (u32::from(src[3]) <<  0);
        self.cache |= u64::from(nw) << (32 - self.bits);
    }

    fn fill32le16(&mut self, src: &[u8]) {
        let nw = (u32::from(src[1]) << 24) |
                 (u32::from(src[0]) << 16) |
                 (u32::from(src[3]) <<  8) |
                 (u32::from(src[2]) <<  0);
        self.cache |= u64::from(nw) << (32 - self.bits);
    }

    fn fill32le32(&mut self, src: &[u8], lsb: bool) {
        let nw = (u32::from(src[3]) << 24) |
                 (u32::from(src[2]) << 16) |
                 (u32::from(src[1]) <<  8) |
                 (u32::from(src[0]) <<  0);
        if lsb {
            self.cache |= u64::from(nw) << self.bits;
        } else {
            self.cache |= u64::from(nw) << (32 - self.bits);
        }
    }

    #[inline(always)]
    fn refill(&mut self) -> BitReaderResult<()> {
        if self.pos >= self.src.len() { return Err(BitstreamEnd) }
        while self.bits <= 32 {
            if self.pos + 4 <= self.src.len() {
                let buf = &self.src[self.pos..];
                match self.mode {
                    BitReaderMode::BE      => self.fill32be  (buf),
                    BitReaderMode::LE16MSB => self.fill32le16(buf),
                    BitReaderMode::LE      => self.fill32le32(buf, true),
                    BitReaderMode::LE32MSB => self.fill32le32(buf, false),
                }
                self.pos  +=  4;
                self.bits += 32;
            } else {
                let mut buf: [u8; 4] = [0, 0, 0, 0];
                let mut newbits: u8 = 0;
                for out in buf.iter_mut().take(3) {
                    if self.pos < self.src.len() {
                        *out = self.src[self.pos];
                        self.pos += 1;
                        newbits += 8;
                    }
                }
                if newbits == 0 { break; }
                match self.mode {
                    BitReaderMode::BE      => self.fill32be  (&buf),
                    BitReaderMode::LE16MSB => self.fill32le16(&buf),
                    BitReaderMode::LE      => self.fill32le32(&buf, true),
                    BitReaderMode::LE32MSB => self.fill32le32(&buf, false),
                }
                self.bits += newbits;
            }
        }
        Ok(())
    }

    #[inline(always)]
    fn read_cache(&mut self, nbits: u8) -> u32 {
        let res = match self.mode {
            BitReaderMode::LE => ((1u64 << nbits) - 1) & self.cache,
            _                 => self.cache >> (64 - nbits),
        };
        res as u32
    }

    fn read_cache_s(&mut self, nbits: u8) -> i32 {
        let res = match self.mode {
            BitReaderMode::LE => ((self.cache as i64) << (64 - nbits)) >> (64 - nbits),
            _                 => (self.cache as i64) >> (64 - nbits),
        };
        res as i32
    }

    #[inline(always)]
    fn skip_cache(&mut self, nbits: u8) {
        match self.mode {
            BitReaderMode::LE => self.cache >>= nbits,
            _                 => self.cache <<= nbits,
        };
        self.bits -= nbits;
    }

    #[inline(always)]
    fn reset_cache(&mut self) {
        self.bits = 0;
        self.cache = 0;
    }

    /// Reads the specified amount of bits as an unsigned value.
    ///
    /// The amount should fit into 32 bits, if you need more then
    /// you should read it as several parts. If the amount of bits
    /// requested to read is larger than the amount of bits left the
    /// call will return [`BitstreamEnd`].
    ///
    /// # Examples
    ///
    /// ```
    /// use nihav_core::io::bitreader::{BitReader,BitReaderMode};
    ///
    /// # use nihav_core::io::bitreader::BitReaderResult;
    /// # fn foo() -> BitReaderResult<u32> {
    /// let bits: [u8; 4] = [ 42, 43, 44, 45 ];
    /// let mut br = BitReader::new(&bits, BitReaderMode::BE);
    /// let value = br.read(17)?;
    /// # Ok(value)
    /// # }
    /// ```
    ///
    /// [`BitstreamEnd`]: ./enum.BitReaderError.html#variant.BitstreamEnd
    #[inline(always)]
    pub fn read(&mut self, nbits: u8) -> BitReaderResult<u32> {
        if nbits == 0 { return Ok(0) }
        if nbits > 32 { return Err(TooManyBitsRequested) }
        if self.bits < nbits {
            self.refill()?;
            if self.bits < nbits { return Err(BitstreamEnd) }
        }
        let res = self.read_cache(nbits);
        self.skip_cache(nbits);
        Ok(res)
    }

    /// Reads the specified amount of bits as a signed value.
    ///
    /// Beside signedness it behaves the same as [`read`].
    ///
    /// [`read`]: #method.read
    pub fn read_s(&mut self, nbits: u8) -> BitReaderResult<i32> {
        if nbits == 0 || nbits > 32 { return Err(TooManyBitsRequested) }
        if self.bits < nbits {
            self.refill()?;
            if self.bits < nbits { return Err(BitstreamEnd) }
        }
        let res = self.read_cache_s(nbits);
        self.skip_cache(nbits);
        Ok(res)
    }

    /// Reads single bit from the stream and interprets it as a boolean value.
    #[inline(always)]
    pub fn read_bool(&mut self) -> BitReaderResult<bool> {
        if self.bits < 1 {
            self.refill()?;
            if self.bits < 1 { return Err(BitstreamEnd) }
        }
        let res = self.read_cache(1);
        self.skip_cache(1);
        Ok(res == 1)
    }

    /// Retrieves the next bits from the stream without advancing.
    ///
    /// If the bitstream is shorter than the amount of bits requested the result is padded with zeroes.
    ///
    /// # Examples
    ///
    /// ```
    /// use nihav_core::io::bitreader::{BitReader,BitReaderMode};
    ///
    /// # use nihav_core::io::bitreader::BitReaderResult;
    /// # fn foo() -> BitReaderResult<u32> {
    /// let bits: [u8; 4] = [ 42, 43, 44, 45 ];
    /// let mut br = BitReader::new(&bits, BitReaderMode::BE);
    /// let peek_value = br.peek(8); // this should return 42
    /// let value = br.read(8)?; // also 42
    /// # Ok(value)
    /// # }
    /// ```
    #[inline(always)]
    pub fn peek(&mut self, nbits: u8) -> u32 {
        if nbits > 32 { return 0 }
        if self.bits < nbits { let _ = self.refill(); }
        self.read_cache(nbits)
    }

    /// Skips the requested amount of bits.
    ///
    /// The amount of bits to skip can be arbitrary large.
    /// If it skips more bits than there are actually in the stream the call will return [`BitstreamEnd`]
    ///
    /// [`read`]: #method.read
    /// [`BitstreamEnd`]: ./enum.BitReaderError.html#variant.BitstreamEnd
    #[inline(always)]
    pub fn skip(&mut self, nbits: u32) -> BitReaderResult<()> {
        if u32::from(self.bits) >= nbits {
            self.skip_cache(nbits as u8);
            return Ok(());
        }
        let mut skip_bits = nbits - u32::from(self.bits);
        self.reset_cache();
        self.pos += ((skip_bits / 32) * 4) as usize;
        skip_bits &= 0x1F;
        if skip_bits > 0 {
            self.refill()?;
            if u32::from(self.bits) < skip_bits {
                return Err(BitstreamEnd);
            }
            self.skip_cache(skip_bits as u8);
        }
        Ok(())
    }

    /// Seeks to the absolute bit position in the stream.
    /// If the requested position lies after the bitstream end the function returns [`TooManyBitsRequested`].
    ///
    /// # Examples
    ///
    /// ```
    /// use nihav_core::io::bitreader::{BitReader,BitReaderMode};
    ///
    /// # use nihav_core::io::bitreader::BitReaderResult;
    /// # fn foo() -> BitReaderResult<u32> {
    /// let bits: [u8; 4] = [ 42, 43, 44, 45 ];
    /// let mut br = BitReader::new(&bits, BitReaderMode::BE);
    /// br.seek(16)?;
    /// let value = br.read(8)?; // this should return 44
    /// # Ok(value)
    /// # }
    /// ```
    ///
    /// [`TooManyBitsRequested`]: ./enum.BitReaderError.html#variant.TooManyBitsRequested
    pub fn seek(&mut self, nbits: u32) -> BitReaderResult<()> {
        if ((nbits + 7) >> 3) as usize > self.src.len() { return Err(TooManyBitsRequested); }
        self.reset_cache();
        self.pos = ((nbits / 32) * 4) as usize;
        self.skip(nbits & 0x1F)
    }

    /// Aligns the bit position to the next byte boundary. If already at byte boundary the function does nothing.
    ///
    /// # Examples
    ///
    /// ```
    /// use nihav_core::io::bitreader::{BitReader,BitReaderMode};
    ///
    /// # use nihav_core::io::bitreader::BitReaderResult;
    /// # fn foo() -> BitReaderResult<()> {
    /// let bits: [u8; 4] = [ 42, 43, 44, 45 ];
    /// let mut br = BitReader::new(&bits, BitReaderMode::BE);
    /// br.skip(17)?; // now reader is at bit position 17
    /// br.align(); // now reader is at bit position 24
    /// br.align(); // now reader is still at bit position 24
    /// # Ok(())
    /// # }
    /// ```
    pub fn align(&mut self) {
        let pos = self.bits & 7;
        if pos != 0 {
            self.skip_cache(pos);
        }
    }
}

/// Returns a variable with `len` amount of low bits in reverse order.
///
/// # Examples
/// ```
/// use nihav_core::io::bitreader::reverse_bits;
/// reverse_bits(0b010101, 6); // the result should be 0b101010
/// ```
pub fn reverse_bits(inval: u32, len: u8) -> u32 {
    if len == 0 { return 0; }
    const REV_TAB: [u8; 16] = [
        0b0000, 0b1000, 0b0100, 0b1100, 0b0010, 0b1010, 0b0110, 0b1110,
        0b0001, 0b1001, 0b0101, 0b1101, 0b0011, 0b1011, 0b0111, 0b1111,
    ];

    let mut ret = 0;
    let mut val = inval;
    for _ in 0..8 {
        ret = (ret << 4) | u32::from(REV_TAB[(val & 0xF) as usize]);
        val >>= 4;
    }
    ret >> (32 - len)
}

#[cfg(test)]
mod test {
    use super::*;

    #[test]
    fn br_works() {
        const DATA: [u8; 18] = [0b00011011; 18];
        let src = &DATA;
        let mut br = BitReader::new(src, BitReaderMode::LE16MSB);

        for _ in 0..8 {
            assert_eq!(br.read(16).unwrap(), 0x1B1B);
        }
        const DATA2: [u8; 1] = [ 0b00011011 ];
        let src = &DATA2;
        let mut br = BitReader::new(src, BitReaderMode::LE);
        assert_eq!(br.read_s(5).unwrap(), -5);
    }
}
