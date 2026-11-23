//! Bitstream writer functionality.
//!
//! Bitstream writer works on `Vec<u8>` and allows to write bits in different modes.
//!
//! # Examples
//!
//! Writing 17-bit value:
//! ```
//! use nihav_core::io::bitwriter::{BitWriter,BitWriterMode};
//!
//! # fn foo() -> Vec<u8> {
//! let mut bw = BitWriter::new(Vec::new(), BitWriterMode::BE);
//! bw.write(42, 17);
//! # bw.end()
//! # }
//! ```

/// Bitstream writing modes.
#[derive(Debug,Clone,Copy,PartialEq)]
pub enum BitWriterMode {
    /// The stream is big endian MSB first.
    BE,
    /// The stream is little endian LSB first.
    LE,
    /// The stream is packed into 16-bit little-endian words MSB first.
    LE16MSB,
    /// The stream is packed into 32-bit little-endian words MSB first.
    LE32MSB,
}

impl BitWriterMode {
    fn is_be(self) -> bool { self != BitWriterMode::LE }
}

/// Bitstream writer.
pub struct BitWriter {
    dst:    Vec<u8>,
    bitbuf: u32,
    bits:   u8,
    start:  usize,
    mode:   BitWriterMode,
}

impl BitWriter {
    /// Creates a new instance of `BitWriter` that will append data to the input vector.
    ///
    /// # Examples
    ///
    /// ```
    /// use nihav_core::io::bitwriter::{BitWriter,BitWriterMode};
    ///
    /// let mut bw = BitWriter::new(Vec::with_capacity(100), BitWriterMode::BE);
    /// ```
    pub fn new(dst: Vec<u8>, mode: BitWriterMode) -> Self {
        let start = dst.len();
        Self {
            dst,
            mode,
            start,
            bitbuf: 0,
            bits:   0,
        }
    }
    /// Writes single zero bit to the output.
    pub fn write0(&mut self) { self.write_bit(false); }
    /// Writes single set bit to the output.
    pub fn write1(&mut self) { self.write_bit(true); }
    /// Writes single bit.
    pub fn write_bit(&mut self, bit: bool) {
        if self.mode.is_be() {
            self.bitbuf |= (bit as u32) << (31 - self.bits);
        } else {
            self.bitbuf |= (bit as u32) << self.bits;
        }
        self.bits += 1;
        self.flush();
    }
    /// Writes `bits` bits of `val` value to the output.
    #[allow(clippy::collapsible_else_if)]
    pub fn write(&mut self, val: u32, bits: u8) {
        if bits == 0 {
            return;
        }
        if self.mode.is_be() {
            if self.bits + bits <= 32 {
                self.bitbuf |= val << (32 - self.bits - bits);
                self.bits += bits;
                self.flush();
            } else {
                let cbits = 32 - self.bits;
                let bits2 = bits - cbits;
                self.write(val >> bits2, cbits);
                self.write(val & ((1 << bits2) - 1), bits2);
            }
        } else {
            if self.bits + bits <= 32 {
                self.bitbuf |= val << self.bits;
                self.bits += bits;
                self.flush();
            } else {
                let cbits = 32 - self.bits;
                let bits2 = bits - cbits;
                self.write(val & ((1 << cbits) - 1), cbits);
                self.write(val >> cbits, bits2);
            }
        }
    }
    /// Writes `bits` bits of signed `val` value to the output.
    pub fn write_s(&mut self, val: i32, bits: u8) {
        self.write((val as u32) & ((1 << bits) - 1), bits);
    }
    /// Tells the amount of bits written so far.
    pub fn tell(&self) -> usize {
        (self.dst.len() - self.start) * 8 + (self.bits as usize)
    }
    fn flush(&mut self) {
        match self.mode {
            BitWriterMode::BE => {
                while self.bits >= 8 {
                    self.dst.push((self.bitbuf >> 24) as u8);
                    self.bitbuf <<= 8;
                    self.bits    -= 8;
                }
            },
            BitWriterMode::LE => {
                while self.bits >= 8 {
                    self.dst.push(self.bitbuf as u8);
                    self.bitbuf >>= 8;
                    self.bits    -= 8;
                }
            },
            BitWriterMode::LE16MSB => {
                while self.bits >= 16 {
                    self.dst.push((self.bitbuf >> 16) as u8);
                    self.dst.push((self.bitbuf >> 24) as u8);
                    self.bitbuf <<= 16;
                    self.bits    -= 16;
                }
            },
            BitWriterMode::LE32MSB => {
                if self.bits == 32 {
                    self.dst.push( self.bitbuf        as u8);
                    self.dst.push((self.bitbuf >>  8) as u8);
                    self.dst.push((self.bitbuf >> 16) as u8);
                    self.dst.push((self.bitbuf >> 24) as u8);
                    self.bitbuf = 0;
                    self.bits   = 0;
                }
            },
        };
    }
    /// Finalises operations and returns the vector containing output data.
    pub fn end(mut self) -> Vec<u8> {
        self.flush();
        if self.bits > 0 {
            match self.mode {
                BitWriterMode::BE => {
                    self.dst.push((self.bitbuf >> 24) as u8);
                },
                BitWriterMode::LE => {
                    self.dst.push(self.bitbuf as u8);
                },
                BitWriterMode::LE16MSB => {
                    self.dst.push((self.bitbuf >> 16) as u8);
                    self.dst.push((self.bitbuf >> 24) as u8);
                },
                BitWriterMode::LE32MSB => {
                    self.dst.push( self.bitbuf        as u8);
                    self.dst.push((self.bitbuf >>  8) as u8);
                    self.dst.push((self.bitbuf >> 16) as u8);
                    self.dst.push((self.bitbuf >> 24) as u8);
                },
            };
        }
        self.dst
    }
}

#[cfg(test)]
mod test {
    use super::*;

    #[test]
    fn bw_works() {
        let mut bw = BitWriter::new(Vec::new(), BitWriterMode::BE);
        bw.write(43, 9);
        let data = bw.end();
        assert_eq!(&data, &[21, 128]);

        let mut bw = BitWriter::new(Vec::new(), BitWriterMode::LE);
        bw.write1();
        bw.write(43, 9);
        let data = bw.end();
        assert_eq!(&data, &[87, 0]);

        let mut bw = BitWriter::new(Vec::new(), BitWriterMode::LE32MSB);
        bw.write(42, 9);
        bw.write(42, 9);
        let data = bw.end();
        assert_eq!(&data, &[0, 128, 10, 21]);
    }
}
