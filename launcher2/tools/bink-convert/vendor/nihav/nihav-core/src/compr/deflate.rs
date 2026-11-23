//! Deflate format (RFC 1951) support.
//!
//! This module provides functionality for decompressing raw deflated streams via [`Inflate`] and gzip files (RFC 1952) via [`gzip_decode`] and compressing raw or zlib streams via [`Deflate`].
//!
//! [`Deflate`]: ./struct.Deflate.html
//! [`Inflate`]: ./struct.Inflate.html
//! [`gzip_decode`]: ./fn.gzip_decode.html
//!
//! # Examples
//!
//! Decompressing full input buffer into sufficiently large output buffer:
//! ```
//! # use nihav_core::compr::DecompressError;
//! use nihav_core::compr::deflate::Inflate;
//!
//! # fn decompress(input: &[u8]) -> Result<(), DecompressError> {
//! # let mut output_buffer = [0u8; 16];
//! let output_length = Inflate::uncompress(input, &mut output_buffer)?;
//! # Ok(())
//! # }
//! ```
//!
//! Decompressing input chunks into portions of output:
//! ```
//! use nihav_core::compr::DecompressError;
//! use nihav_core::compr::deflate::Inflate;
//!
//! # fn decompress(input_data: &[u8]) -> Result<(), DecompressError> {
//! let mut inflate = Inflate::new();
//! let mut dst_buf: Vec<u8> = Vec::new();
//! let mut output_chunk = [0u8; 1024];
//! for src in input_data.chunks(512) {
//!     let mut repeat = false;
//!     loop {
//!         let ret = inflate.decompress_data(src, &mut output_chunk, repeat);
//!         match ret {
//!             Ok(len) => { // we got a buffer decoded successfully to the end
//!                 dst_buf.extend_from_slice(&output_chunk[..len]);
//!                 break;
//!             },
//!             Err(DecompressError::ShortData) => { // this block of data was fully read
//!                 break;
//!             },
//!             Err(DecompressError::OutputFull) => {
//!                 // the output buffer is full, flush it and continue decoding the same block
//!                 repeat = true;
//!                 dst_buf.extend_from_slice(&output_chunk);
//!             },
//!             Err(err) => {
//!                 return Err(err);
//!             },
//!         }
//!     }
//! }
//! # Ok(())
//! # }
//! ```
//!
//! Compressing input buffer into zlib stream:
//! ```
//! use nihav_core::compr::deflate::{Deflate, DeflateMode, DeflateWriter};
//!
//! # fn compress(input: &[u8]) {
//! let output = Vec::with_capacity(input.len() * 65540 / 65535 + 6);
//! let mut writer = DeflateWriter::new(output);
//! let mut compr = Deflate::new(DeflateMode::Fast);
//! compr.write_zlib_header(&mut writer);
//! compr.compress(input, &mut writer);
//! compr.compress_end(&mut writer);
//! let output = writer.end();
//! # }
//! ```

use crate::options::NAOptionDefinitionType;
use crate::io::byteio::*;
use crate::io::bitreader::*;
use crate::io::codebook::*;
use super::*;

const NUM_LITERALS: usize = 287;
const NUM_DISTS:    usize = 32;

struct FixedLenCodeReader {}

impl CodebookDescReader<u16> for FixedLenCodeReader {
    fn bits(&mut self, idx: usize) -> u8  {
        if      idx < 144 { 8 }
        else if idx < 256 { 9 }
        else if idx < 280 { 7 }
        else              { 8 }
    }
    #[allow(clippy::identity_op)]
    fn code(&mut self, idx: usize) -> u32 {
        let base = idx as u32;
        let bits = self.bits(idx);
        if      idx < 144 { reverse_bits(base + 0x30, bits) }
        else if idx < 256 { reverse_bits(base + 0x190 - 144, bits) }
        else if idx < 280 { reverse_bits(base + 0x000 - 256, bits) }
        else              { reverse_bits(base + 0xC0 - 280, bits) }
    }
    fn sym (&mut self, idx: usize) -> u16 { idx as u16 }
    fn len(&mut self) -> usize { NUM_LITERALS + 1 }
}

#[derive(Clone,Copy,Default)]
struct BitReaderState {
    pos:            usize,
    bitbuf:         u32,
    bits:           u8,
}

struct CurrentSource<'a> {
    src:            &'a [u8],
    br:             BitReaderState,
}

impl<'a> CurrentSource<'a> {
    fn new(src: &'a [u8], br: BitReaderState) -> Self {
        let mut newsrc = Self { src, br };
        newsrc.br.pos = 0;
        newsrc.refill();
        newsrc
    }
    fn reinit(src: &'a [u8], br: BitReaderState) -> Self {
        let mut newsrc = Self { src, br };
        newsrc.refill();
        newsrc
    }
    fn refill(&mut self) {
        while (self.br.bits <= 24) && (self.br.pos < self.src.len()) {
            self.br.bitbuf |= u32::from(self.src[self.br.pos]) << self.br.bits;
            self.br.bits += 8;
            self.br.pos += 1;
        }
    }
    fn skip_cache(&mut self, nbits: u8) {
        self.br.bitbuf >>= nbits;
        self.br.bits    -= nbits;
    }
    fn read(&mut self, nbits: u8) -> BitReaderResult<u32> {
        if nbits == 0 { return Ok(0); }
        if nbits > 16 { return Err(BitReaderError::TooManyBitsRequested); }
        if self.br.bits < nbits {
            self.refill();
            if self.br.bits < nbits { return Err(BitReaderError::BitstreamEnd); }
        }
        let ret = self.br.bitbuf & ((1 << nbits) - 1);
        self.skip_cache(nbits);
        Ok(ret)
    }
    fn read_bool(&mut self) -> BitReaderResult<bool> {
        if self.br.bits == 0 {
            self.refill();
            if self.br.bits == 0 { return Err(BitReaderError::BitstreamEnd); }
        }
        let ret = (self.br.bitbuf & 1) != 0;
        self.skip_cache(1);
        Ok(ret)
    }
    fn peek(&mut self, nbits: u8) -> u32 {
        if nbits == 0 || nbits > 16 { return 0; }
        if self.br.bits < nbits {
            self.refill();
        }
        self.br.bitbuf & ((1 << nbits) - 1)
    }
    fn skip(&mut self, nbits: u32) -> BitReaderResult<()> {
        if u32::from(self.br.bits) >= nbits {
            self.skip_cache(nbits as u8);
        } else {
            unreachable!();
        }
        Ok(())
    }
    fn skip_bytes(&mut self, nbytes: usize) -> BitReaderResult<()> {
        self.align();
        let cached = usize::from(self.br.bits / 8);
        if nbytes <= cached {
            self.skip((nbytes as u32) * 8)?;
        } else {
            self.skip((cached as u32) * 8)?;
            self.br.bits = 0;
            self.br.bitbuf = 0;
            self.br.pos += nbytes - cached;
            if self.br.pos > self.src.len() {
                return Err(BitReaderError::BitstreamEnd);
            }
            self.refill();
        }
        Ok(())
    }
    fn align(&mut self) {
        let b = self.br.bits & 7;
        if b != 0 {
            self.skip_cache(b);
        }
    }
    fn left(&self) -> isize {
        ((self.src.len() as isize) - (self.br.pos as isize)) * 8 + (self.br.bits as isize)
    }
    fn tell(&self) -> usize {
        self.br.pos - usize::from(self.br.bits / 8)
    }
}

impl<'a, S: Copy> CodebookReader<S> for CurrentSource<'a> {
    fn read_cb(&mut self, cb: &Codebook<S>) -> CodebookResult<S> {
        let mut esc = true;
        let mut idx = 0;
        let mut lut_bits = cb.lut_bits;
        let orig_br = self.br;
        while esc {
            let lut_idx = (self.peek(lut_bits) as usize) + idx;
            if cb.table[lut_idx] == TABLE_FILL_VALUE { return Err(CodebookError::InvalidCode); }
            let bits = cb.table[lut_idx] & 0x7F;
            esc  = (cb.table[lut_idx] & 0x80) != 0;
            idx  = (cb.table[lut_idx] >> 8) as usize;
            let skip_bits = if esc { u32::from(lut_bits) } else { bits };
            if (skip_bits as isize) > self.left() {
                self.br = orig_br;
                self.refill();
                return Err(CodebookError::MemoryError);
            }
            self.skip(skip_bits).unwrap();
            lut_bits = bits as u8;
        }
        Ok(cb.syms[idx])
    }
}

enum InflateState {
    Start,
    BlockStart,
    BlockMode,
    StaticBlockLen,
    StaticBlockInvLen(u32),
    StaticBlockCopy(usize),
    FixedBlock,
    FixedBlockLengthExt(usize, u8),
    FixedBlockDist(usize),
    FixedBlockDistExt(usize, usize, u8),
    FixedBlockCopy(usize, usize),
    FixedBlockLiteral(u8),
    DynBlockHlit,
    DynBlockHdist,
    DynBlockHclen,
    DynLengths(usize),
    DynCodeLengths,
    DynCodeLengthsAdd(usize),
    DynBlock,
    DynBlockLengthExt(usize, u8),
    DynBlockDist(usize),
    DynBlockDistExt(usize, usize, u8),
    DynCopy(usize, usize),
    DynBlockLiteral(u8),
    End,
}

/// The decompressor for deflated streams (RFC 1951).
pub struct Inflate {
    br:             BitReaderState,
    fix_len_cb:     Codebook<u16>,

    buf:            [u8; 65536],
    bpos:           usize,
    output_idx:     usize,
    full_pos:       usize,

    state:          InflateState,
    final_block:    bool,
    hlit:           usize,
    hdist:          usize,
    dyn_len_cb:     Option<Codebook<u32>>,
    dyn_lit_cb:     Option<Codebook<u32>>,
    dyn_dist_cb:    Option<Codebook<u32>>,
    len_lengths:    [u8; 19],
    all_lengths:    [u8; NUM_LITERALS + NUM_DISTS],
    cur_len_idx:    usize,
}

const LENGTH_ADD_BITS: [u8; 29] = [
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1,
    1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
    4, 4, 4, 4, 5, 5, 5, 5, 0
];
const LENGTH_BASE: [u16; 29] = [
     3,   4,   5,   6,   7,   8,   9,  10,  11,  13,
    15,  17,  19,  23,  27,  31,  35,  43,  51,  59,
    67,  83,  99, 115, 131, 163, 195, 227, 258
];
const DIST_ADD_BITS: [u8; 30] = [
    0,  0,  0,  0,  1,  1,  2,  2,  3,  3,
    4,  4,  5,  5,  6,  6,  7,  7,  8,  8,
    9,  9, 10, 10, 11, 11, 12, 12, 13, 13
];
const DIST_BASE: [u16; 30] = [
       1,    2,    3,    4,    5,    7,    9,    13,    17,    25,
      33,   49,   65,   97,  129,  193,  257,   385,   513,   769,
    1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
];
const LEN_RECODE: [usize; 19] = [
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
];
const REPEAT_BITS: [u8; 3] = [ 2, 3, 7 ];
const REPEAT_BASE: [u8; 3] = [ 3, 3, 11 ];

macro_rules! read_bits {
    ($self: expr, $csrc: expr, $bits: expr) => ({
        if $csrc.left() < $bits as isize {
            $self.br = $csrc.br;
            return Err(DecompressError::ShortData);
        }
        $csrc.read($bits).unwrap()
    })
}

macro_rules! read_cb {
    ($self: expr, $csrc: expr, $cb: expr) => ({
        let ret = $csrc.read_cb($cb);
        if let Err(CodebookError::MemoryError) = ret {
            $self.br = $csrc.br;
            return Err(DecompressError::ShortData);
        }
        match ret {
            Ok(val) => val,
            Err(_)  => {
                $self.state = InflateState::End;
                return Err(DecompressError::InvalidData);
            },
        }
    })
}

impl Inflate {
    /// Creates a new instance of `Inflate` struct.
    pub fn new() -> Self {
        let mut cr = FixedLenCodeReader {};
        let fix_len_cb = Codebook::new(&mut cr, CodebookMode::LSB).unwrap();
        Self {
            br:             BitReaderState::default(),
            fix_len_cb,

            buf:            [0; 65536],
            bpos:           0,
            output_idx:     0,
            full_pos:       0,

            state:          InflateState::Start,
            final_block:    false,
            dyn_len_cb:     None,
            dyn_lit_cb:     None,
            dyn_dist_cb:    None,
            hlit:           0,
            hdist:          0,
            len_lengths:    [0; 19],
            all_lengths:    [0; NUM_LITERALS + NUM_DISTS],
            cur_len_idx:    0,
        }
    }
    fn put_literal(&mut self, val: u8) {
        self.buf[self.bpos] = val;
        self.bpos = (self.bpos + 1) & (self.buf.len() - 1);
        self.full_pos += 1;
    }
    fn lz_copy(&mut self, offset: usize, len: usize, dst: &mut [u8]) -> DecompressResult<()> {
        let mask = self.buf.len() - 1;
        if offset > self.full_pos {
            return Err(DecompressError::InvalidData);
        }
        let cstart = (self.bpos.wrapping_sub(offset)) & mask;
        for (i, out) in dst[..len].iter_mut().enumerate() {
            self.buf[(self.bpos + i) & mask] = self.buf[(cstart + i) & mask];
            *out = self.buf[(cstart + i) & mask];
        }
        self.bpos = (self.bpos + len) & mask;
        self.full_pos += len;
        Ok(())
    }
    /// Sets custom history for decoding an update for already decoded data.
    pub fn set_dict(&mut self, dict: &[u8]) {
        let len = dict.len().min(self.buf.len());
        let start = dict.len() - len;
        self.buf[..len].copy_from_slice(&dict[start..]);
        self.bpos = len;
        self.full_pos = len;
    }
    /// Reports whether decoder has finished decoding the input.
    pub fn is_finished(&self) -> bool {
        matches!(self.state, InflateState::End)
    }
    /// Reports the current amount of bytes output into the destination buffer after the last run.
    pub fn get_current_output_size(&self) -> usize { self.output_idx }
    /// Reports the total amount of bytes decoded so far.
    pub fn get_total_output_size(&self) -> usize { self.bpos }
    /// Tries to decompress input data and write it to the output buffer.
    ///
    /// Since the decompressor can work with arbitrary input and output chunks its return value may have several meanings:
    /// * `Ok(len)` means the stream has been fully decoded and then number of bytes output into the destination buffer is returned.
    /// * [`DecompressError::ShortData`] means the input stream has been fully read but more data is needed.
    /// * [`DecompressError::OutputFull`] means the output buffer is full and should be flushed. Then decoding should continue on the same input block with `continue_block` parameter set to `true`.
    ///
    /// [`DecompressError::ShortData`]: ../enum.DecompressError.html#variant.ShortData
    /// [`DecompressError::OutputFull`]: ../enum.DecompressError.html#variant.OutputFull
    pub fn decompress_data(&mut self, src: &[u8], dst: &mut [u8], continue_block: bool) -> DecompressResult<usize> {
        self.decompress_data_internal(src, dst, continue_block, false)
    }
    /// Tries to decompress whole input chunk to the output buffer.
    pub fn decompress_block(&mut self, src: &[u8], dst: &mut [u8]) -> DecompressResult<usize> {
        self.decompress_data_internal(src, dst, false, true)
    }
    #[allow(clippy::comparison_chain)]
    fn decompress_data_internal(&mut self, src: &[u8], dst: &mut [u8], continue_block: bool, do_one_block: bool) -> DecompressResult<usize> {
        if src.is_empty() || dst.is_empty() {
            return Err(DecompressError::InvalidArgument);
        }
        let mut csrc = if !continue_block {
                CurrentSource::new(src, self.br)
            } else {
                self.output_idx = 0;
                CurrentSource::reinit(src, self.br)
            };
        if do_one_block {
            self.output_idx = 0;
        }
        // check for zlib stream header
        if let (&InflateState::Start, true) = (&self.state, src.len() > 2) {
            let cm    = src[0] & 0xF;
            let cinfo = src[0] >> 4;
            let hdr   = (u16::from(src[0]) << 8) | u16::from(src[1]);
            if cm == 8 && cinfo <= 7 && (hdr % 31) == 0 {
                csrc.skip(16).unwrap();
            }
        }
        'main: loop {
            match self.state {
                InflateState::Start | InflateState::BlockStart => {
                    if csrc.left() == 0 {
                        if do_one_block {
                            return Ok(self.output_idx);
                        }
                        self.br = csrc.br;
                        return Err(DecompressError::ShortData);
                    }
                    self.final_block = csrc.read_bool().unwrap();
                    self.state = InflateState::BlockMode;
                },
                InflateState::BlockMode => {
                    let bmode = read_bits!(self, csrc, 2);
                    match bmode {
                        0 => {
                            csrc.align();
                            self.state = InflateState::StaticBlockLen;
                        },
                        1 => { self.state = InflateState::FixedBlock; },
                        2 => { self.state = InflateState::DynBlockHlit; },
                        _ => {
                            self.state = InflateState::End;
                            return Err(DecompressError::InvalidHeader);
                        },
                    };
                },
                InflateState::StaticBlockLen => {
                    let len = read_bits!(self, csrc, 16);
                    self.state = InflateState::StaticBlockInvLen(len);
                },
                InflateState::StaticBlockInvLen(len) => {
                    let inv_len = read_bits!(self, csrc, 16);
                    if (len ^ inv_len) != 0xFFFF {
                        self.state = InflateState::End;
                        return Err(DecompressError::InvalidHeader);
                    }
                    self.state = InflateState::StaticBlockCopy(len as usize);
                },
                InflateState::StaticBlockCopy(len) => {
                    for i in 0..len {
                        if csrc.left() < 8 {
                            self.br = csrc.br;
                            self.state = InflateState::StaticBlockCopy(len - i);
                            return Err(DecompressError::ShortData);
                        }
                        let val = csrc.read(8).unwrap() as u8;
                        self.put_literal(val);
                    }
                    self.state = InflateState::BlockStart;
                }
                InflateState::FixedBlock => {
                    let val = read_cb!(self, csrc, &self.fix_len_cb);
                    if val < 256 {
                        if self.output_idx >= dst.len() {
                            self.br = csrc.br;
                            self.state = InflateState::FixedBlockLiteral(val as u8);
                            return Err(DecompressError::OutputFull);
                        }
                        self.put_literal(val as u8);
                        dst[self.output_idx] = val as u8;
                        self.output_idx += 1;
                    } else if val == 256 {
                        if self.final_block {
                            self.state = InflateState::End;
                            return Ok(self.output_idx);
                        } else {
                            self.state = InflateState::BlockStart;
                        }
                    } else {
                        let len_idx = (val - 257) as usize;
                        if len_idx >= LENGTH_BASE.len() {
                            self.state = InflateState::End;
                            return Err(DecompressError::InvalidData);
                        }
                        let len_bits = LENGTH_ADD_BITS[len_idx];
                        let add_base = LENGTH_BASE[len_idx] as usize;
                        if len_bits > 0 {
                            self.state = InflateState::FixedBlockLengthExt(add_base, len_bits);
                        } else {
                            self.state = InflateState::FixedBlockDist(add_base);
                        }
                    }
                },
                InflateState::FixedBlockLiteral(sym) => {
                    if self.output_idx >= dst.len() {
                        self.br = csrc.br;
                        return Err(DecompressError::OutputFull);
                    }
                    self.put_literal(sym);
                    dst[self.output_idx] = sym;
                    self.output_idx += 1;
                    self.state = InflateState::FixedBlock;
                },
                InflateState::FixedBlockLengthExt(base, bits) => {
                    let add = read_bits!(self, csrc, bits) as usize;
                    self.state = InflateState::FixedBlockDist(base + add);
                },
                InflateState::FixedBlockDist(length) => {
                    let dist_idx = reverse_bits(read_bits!(self, csrc, 5), 5) as usize;
                    if dist_idx >= DIST_BASE.len() {
                        self.state = InflateState::End;
                        return Err(DecompressError::InvalidData);
                    }
                    let dist_bits = DIST_ADD_BITS[dist_idx];
                    let dist_base = DIST_BASE[dist_idx] as usize;
                    if dist_bits == 0 {
                        self.state = InflateState::FixedBlockCopy(length, dist_base);
                    } else {
                        self.state = InflateState::FixedBlockDistExt(length, dist_base, dist_bits);
                    }
                },
                InflateState::FixedBlockDistExt(length, base, bits) => {
                    let add = read_bits!(self, csrc, bits) as usize;
                    self.state = InflateState::FixedBlockCopy(length, base + add);
                },
                InflateState::FixedBlockCopy(length, dist) => {
                    if self.output_idx + length > dst.len() {
                        let copy_size = dst.len() - self.output_idx;
                        let ret = self.lz_copy(dist, copy_size, &mut dst[self.output_idx..]);
                        if ret.is_err() {
                            self.state = InflateState::End;
                            return Err(DecompressError::InvalidData);
                        }
                        self.output_idx += copy_size;
                        self.br = csrc.br;
                        self.state = InflateState::FixedBlockCopy(length - copy_size, dist);
                        return Err(DecompressError::OutputFull);
                    }
                    let ret = self.lz_copy(dist, length, &mut dst[self.output_idx..]);
                    if ret.is_err() {
                        self.state = InflateState::End;
                        return Err(DecompressError::InvalidData);
                    }
                    self.output_idx += length;
                    self.state = InflateState::FixedBlock;
                }
                InflateState::DynBlockHlit => {
                    self.hlit = (read_bits!(self, csrc, 5) as usize) + 257;
                    if self.hlit >= 287 {
                        self.state = InflateState::End;
                        return Err(DecompressError::InvalidHeader);
                    }
                    self.state = InflateState::DynBlockHdist;
                }
                InflateState::DynBlockHdist => {
                    self.hdist = (read_bits!(self, csrc, 5) as usize) + 1;
                    self.state = InflateState::DynBlockHclen;
                },
                InflateState::DynBlockHclen => {
                    let hclen = (read_bits!(self, csrc, 4) as usize) + 4;
                    self.cur_len_idx = 0;
                    self.len_lengths = [0; 19];
                    self.all_lengths = [0; NUM_LITERALS + NUM_DISTS];
                    self.state = InflateState::DynLengths(hclen);
                },
                InflateState::DynLengths(len) => {
                    for i in 0..len {
                        if csrc.left() < 3 {
                            self.br = csrc.br;
                            self.state = InflateState::DynLengths(len - i);
                            return Err(DecompressError::ShortData);
                        }
                        self.len_lengths[LEN_RECODE[self.cur_len_idx]] = csrc.read(3).unwrap() as u8;
                        self.cur_len_idx += 1;
                    }
                    let mut len_codes = [ShortCodebookDesc { code: 0, bits: 0 }; 19];
                    lengths_to_codes(&self.len_lengths, &mut len_codes)?;
                    let mut cr = ShortCodebookDescReader::new(&len_codes);
                    let ret = Codebook::new(&mut cr, CodebookMode::LSB);
                    if ret.is_err() {
                        self.state = InflateState::End;
                        return Err(DecompressError::InvalidHeader);
                    }
                    self.dyn_len_cb = Some(ret.unwrap());
                    self.cur_len_idx = 0;
                    self.state = InflateState::DynCodeLengths;
                },
                InflateState::DynCodeLengths => {
                    if let Some(ref len_cb) = self.dyn_len_cb {
                        while self.cur_len_idx < self.hlit + self.hdist {
                            let ret = csrc.read_cb(len_cb);
                            let val = match ret {
                                    Ok(val) => val,
                                    Err(CodebookError::MemoryError) => {
                                        self.br = csrc.br;
                                        return Err(DecompressError::ShortData);
                                    },
                                    Err(_) => {
                                        self.state = InflateState::End;
                                        return Err(DecompressError::InvalidHeader);
                                    },
                                };
                            if val < 16 {
                                self.all_lengths[self.cur_len_idx] = val as u8;
                                self.cur_len_idx += 1;
                            } else {
                                let idx = (val as usize) - 16;
                                if idx > 2 {
                                    self.state = InflateState::End;
                                    return Err(DecompressError::InvalidHeader);
                                }
                                self.state = InflateState::DynCodeLengthsAdd(idx);
                                continue 'main;
                            }
                        }
                        let (lit_lengths, dist_lengths) = self.all_lengths.split_at(self.hlit);

                        let mut lit_codes = [ShortCodebookDesc { code: 0, bits: 0 }; NUM_LITERALS];
                        lengths_to_codes(lit_lengths, &mut lit_codes)?;
                        let mut cr = ShortCodebookDescReader::new(&lit_codes);
                        let ret = Codebook::new(&mut cr, CodebookMode::LSB);
                        if ret.is_err() { return Err(DecompressError::InvalidHeader); }
                        self.dyn_lit_cb = Some(ret.unwrap());

                        let mut dist_codes = [ShortCodebookDesc { code: 0, bits: 0 }; NUM_DISTS];
                        lengths_to_codes(&dist_lengths[..self.hdist], &mut dist_codes)?;
                        let mut cr = ShortCodebookDescReader::new(&dist_codes);
                        let ret = Codebook::new(&mut cr, CodebookMode::LSB);
                        if ret.is_err() { return Err(DecompressError::InvalidHeader); }
                        self.dyn_dist_cb = Some(ret.unwrap());

                        self.state = InflateState::DynBlock;
                    } else {
                        unreachable!();
                    }
                },
                InflateState::DynCodeLengthsAdd(mode) => {
                    let base = REPEAT_BASE[mode] as usize;
                    let bits = REPEAT_BITS[mode];
                    let len = base + read_bits!(self, csrc, bits) as usize;
                    if self.cur_len_idx + len > self.hlit + self.hdist {
                        self.state = InflateState::End;
                        return Err(DecompressError::InvalidHeader);
                    }
                    let rpt = if mode == 0 {
                            if self.cur_len_idx == 0 {
                                self.state = InflateState::End;
                                return Err(DecompressError::InvalidHeader);
                            }
                            self.all_lengths[self.cur_len_idx - 1]
                        } else {
                            0
                        };
                    for _ in 0..len {
                        self.all_lengths[self.cur_len_idx] = rpt;
                        self.cur_len_idx += 1;
                    }
                    self.state = InflateState::DynCodeLengths;
                },
                InflateState::DynBlock => {
                    if let Some(ref lit_cb) = self.dyn_lit_cb {
                        let val = read_cb!(self, csrc, lit_cb);
                        if val < 256 {
                            if self.output_idx >= dst.len() {
                                self.br = csrc.br;
                                self.state = InflateState::DynBlockLiteral(val as u8);
                                return Err(DecompressError::OutputFull);
                            }
                            self.put_literal(val as u8);
                            dst[self.output_idx] = val as u8;
                            self.output_idx += 1;
                        } else if val == 256 {
                            if self.final_block {
                                self.state = InflateState::End;
                                return Ok(self.output_idx);
                            } else {
                                self.state = InflateState::BlockStart;
                            }
                        } else {
                            let len_idx = (val - 257) as usize;
                            if len_idx >= LENGTH_BASE.len() {
                                self.state = InflateState::End;
                                return Err(DecompressError::InvalidData);
                            }
                            let len_bits = LENGTH_ADD_BITS[len_idx];
                            let add_base = LENGTH_BASE[len_idx] as usize;
                            if len_bits > 0 {
                                self.state = InflateState::DynBlockLengthExt(add_base, len_bits);
                            } else {
                                self.state = InflateState::DynBlockDist(add_base);
                            }
                        }
                    } else {
                        unreachable!();
                    }
                },
                InflateState::DynBlockLiteral(sym) => {
                    if self.output_idx >= dst.len() {
                        self.br = csrc.br;
                        return Err(DecompressError::OutputFull);
                    }
                    self.put_literal(sym);
                    dst[self.output_idx] = sym;
                    self.output_idx += 1;
                    self.state = InflateState::DynBlock;
                },
                InflateState::DynBlockLengthExt(base, bits) => {
                    let add = read_bits!(self, csrc, bits) as usize;
                    self.state = InflateState::DynBlockDist(base + add);
                },
                InflateState::DynBlockDist(length) => {
                    if let Some(ref dist_cb) = self.dyn_dist_cb {
                        let dist_idx = read_cb!(self, csrc, dist_cb) as usize;
                        if dist_idx >= DIST_BASE.len() {
                            self.state = InflateState::End;
                            return Err(DecompressError::InvalidData);
                        }
                        let dist_bits = DIST_ADD_BITS[dist_idx];
                        let dist_base = DIST_BASE[dist_idx] as usize;
                        if dist_bits == 0 {
                            self.state = InflateState::DynCopy(length, dist_base);
                        } else {
                            self.state = InflateState::DynBlockDistExt(length, dist_base, dist_bits);
                        }
                    } else {
                        unreachable!();
                    }
                },
                InflateState::DynBlockDistExt(length, base, bits) => {
                    let add = read_bits!(self, csrc, bits) as usize;
                    self.state = InflateState::DynCopy(length, base + add);
                },
                InflateState::DynCopy(length, dist) => {
                    if self.output_idx + length > dst.len() {
                        let copy_size = dst.len() - self.output_idx;
                        let ret = self.lz_copy(dist, copy_size, &mut dst[self.output_idx..]);
                        if ret.is_err() {
                            self.state = InflateState::End;
                            return Err(DecompressError::InvalidData);
                        }
                        self.output_idx += copy_size;
                        self.br = csrc.br;
                        self.state = InflateState::DynCopy(length - copy_size, dist);
                        return Err(DecompressError::OutputFull);
                    }
                    let ret = self.lz_copy(dist, length, &mut dst[self.output_idx..]);
                    if ret.is_err() {
                        self.state = InflateState::End;
                        return Err(DecompressError::InvalidData);
                    }
                    self.output_idx += length;
                    self.state = InflateState::DynBlock;
                }
                InflateState::End => {
                    return Ok(0);
                },
            }
        }
    }
    /// Resets decoder state.
    pub fn reset(&mut self) {
        self.bpos = 0;
        self.output_idx = 0;
        self.full_pos = 0;
        self.state = InflateState::Start;
    }

    #[allow(clippy::comparison_chain)]
    /// Decompresses input data into output returning the uncompressed data length.
    pub fn uncompress(src: &[u8], dst: &mut [u8]) -> DecompressResult<usize> {
        let mut csrc = CurrentSource::new(src, BitReaderState::default());
        if src.len() > 2 {
            let cm    = src[0] & 0xF;
            let cinfo = src[0] >> 4;
            let hdr   = (u16::from(src[0]) << 8) | u16::from(src[1]);
            if cm == 8 && cinfo <= 7 && (hdr % 31) == 0 {
                csrc.skip(16).unwrap();
            }
        }

        let mut fix_len_cb = None;

        let mut dst_idx = 0;
        let mut final_block = false;
        while !final_block {
            final_block = csrc.read_bool()?;

            let bmode = csrc.read(2)?;
            match bmode {
                0 => {
                                  csrc.align();
                    let len     = csrc.read(16)? as usize;
                    let inv_len = csrc.read(16)? as usize;
                    if (len ^ inv_len) != 0xFFFF {
                        return Err(DecompressError::InvalidHeader);
                    }
                    let src_pos = csrc.tell();
                    if src_pos + len > src.len() {
                        return Err(DecompressError::ShortData);
                    }
                    if dst_idx + len > dst.len() {
                        return Err(DecompressError::OutputFull);
                    }
                    dst[dst_idx..][..len].copy_from_slice(&src[src_pos..][..len]);
                    dst_idx += len;
                                  csrc.skip_bytes(len)?;
                },
                1 => {
                    if fix_len_cb.is_none() {
                        let mut cr = FixedLenCodeReader {};
                        fix_len_cb = Some(Codebook::new(&mut cr, CodebookMode::LSB).unwrap());
                    }
                    if let Some(ref len_cb) = &fix_len_cb {
                        loop {
                            let val = csrc.read_cb(len_cb)?;
                            if val < 256 {
                                if dst_idx >= dst.len() {
                                    return Err(DecompressError::OutputFull);
                                }
                                dst[dst_idx] = val as u8;
                                dst_idx += 1;
                            } else if val == 256 {
                                break;
                            } else {
                                let len_idx = (val - 257) as usize;
                                if len_idx >= LENGTH_BASE.len() {
                                    return Err(DecompressError::InvalidData);
                                }
                                let len_bits = LENGTH_ADD_BITS[len_idx];
                                let mut length = LENGTH_BASE[len_idx] as usize;
                                if len_bits > 0 {
                                    length += csrc.read(len_bits)? as usize;
                                }
                                let dist_idx = reverse_bits(csrc.read(5)?, 5) as usize;
                                if dist_idx >= DIST_BASE.len() {
                                    return Err(DecompressError::InvalidData);
                                }
                                let dist_bits = DIST_ADD_BITS[dist_idx];
                                let mut dist = DIST_BASE[dist_idx] as usize;
                                if dist_bits > 0 {
                                    dist += csrc.read(dist_bits)? as usize;
                                }

                                if dst_idx + length > dst.len() {
                                    return Err(DecompressError::OutputFull);
                                }
                                if dist > dst_idx {
                                    return Err(DecompressError::InvalidData);
                                }
                                lz_copy(dst, dst_idx, dist, length);
                                dst_idx += length;
                            }
                        }
                    } else {
                        unreachable!();
                    }
                },
                2 => {
                    let hlit = csrc.read(5)? as usize + 257;
                    if hlit >= 287 {
                        return Err(DecompressError::InvalidHeader);
                    }
                    let hdist = csrc.read(5)? as usize + 1;
                    let hclen = csrc.read(4)? as usize + 4;
                    let mut len_lengths = [0; 19];
                    let mut all_lengths = [0; NUM_LITERALS + NUM_DISTS];

                    for cur_len_idx in 0..hclen {
                        len_lengths[LEN_RECODE[cur_len_idx]] = csrc.read(3)? as u8;
                    }
                    let mut len_codes = [ShortCodebookDesc { code: 0, bits: 0 }; 19];
                    lengths_to_codes(&len_lengths, &mut len_codes)?;
                    let mut cr = ShortCodebookDescReader::new(&len_codes);
                    let ret = Codebook::new(&mut cr, CodebookMode::LSB);
                    if ret.is_err() {
                        return Err(DecompressError::InvalidHeader);
                    }
                    let dyn_len_cb = ret.unwrap();

                    let mut cur_len_idx = 0;
                    while cur_len_idx < hlit + hdist {
                        let val = csrc.read_cb(&dyn_len_cb)?;
                        if val < 16 {
                            all_lengths[cur_len_idx] = val as u8;
                            cur_len_idx += 1;
                        } else {
                            let mode = (val as usize) - 16;
                            if mode > 2 {
                                return Err(DecompressError::InvalidHeader);
                            }
                            let base = REPEAT_BASE[mode] as usize;
                            let bits = REPEAT_BITS[mode];
                            let len = base + (csrc.read(bits)? as usize);
                            if cur_len_idx + len > hlit + hdist {
                                return Err(DecompressError::InvalidHeader);
                            }
                            let rpt = if mode == 0 {
                                    if cur_len_idx == 0 {
                                        return Err(DecompressError::InvalidHeader);
                                    }
                                    all_lengths[cur_len_idx - 1]
                                } else {
                                    0
                                };
                            for _ in 0..len {
                                all_lengths[cur_len_idx] = rpt;
                                cur_len_idx += 1;
                            }
                        }
                    }
                    let (lit_lengths, dist_lengths) = all_lengths.split_at(hlit);

                    let mut lit_codes = [ShortCodebookDesc { code: 0, bits: 0 }; NUM_LITERALS];
                    lengths_to_codes(lit_lengths, &mut lit_codes)?;
                    let mut cr = ShortCodebookDescReader::new(&lit_codes);
                    let ret = Codebook::new(&mut cr, CodebookMode::LSB);
                    if ret.is_err() { return Err(DecompressError::InvalidHeader); }
                    let dyn_lit_cb = ret.unwrap();

                    let mut dist_codes = [ShortCodebookDesc { code: 0, bits: 0 }; NUM_DISTS];
                    lengths_to_codes(&dist_lengths[..hdist], &mut dist_codes)?;
                    let mut cr = ShortCodebookDescReader::new(&dist_codes);
                    let ret = Codebook::new(&mut cr, CodebookMode::LSB);
                    if ret.is_err() { return Err(DecompressError::InvalidHeader); }
                    let dyn_dist_cb = ret.unwrap();

                    loop {
                        let val = csrc.read_cb(&dyn_lit_cb)?;
                        if val < 256 {
                            if dst_idx >= dst.len() {
                                return Err(DecompressError::OutputFull);
                            }
                            dst[dst_idx] = val as u8;
                            dst_idx += 1;
                        } else if val == 256 {
                            break;
                        } else {
                            let len_idx = (val - 257) as usize;
                            if len_idx >= LENGTH_BASE.len() {
                                return Err(DecompressError::InvalidData);
                            }
                            let len_bits = LENGTH_ADD_BITS[len_idx];
                            let mut length = LENGTH_BASE[len_idx] as usize;
                            if len_bits > 0 {
                                length += csrc.read(len_bits)? as usize;
                            }

                            let dist_idx = csrc.read_cb(&dyn_dist_cb)? as usize;
                            if dist_idx >= DIST_BASE.len() {
                                return Err(DecompressError::InvalidData);
                            }
                            let dist_bits = DIST_ADD_BITS[dist_idx];
                            let mut dist = DIST_BASE[dist_idx] as usize;
                            if dist_bits > 0 {
                                dist += csrc.read(dist_bits)? as usize;
                            }

                            if dst_idx + length > dst.len() {
                                return Err(DecompressError::OutputFull);
                            }
                            if dist > dst_idx {
                                return Err(DecompressError::InvalidData);
                            }
                            lz_copy(dst, dst_idx, dist, length);
                            dst_idx += length;
                        }
                    }
                },
                _ => return Err(DecompressError::InvalidHeader),
            };
        }
        Ok(dst_idx)
    }
}

impl Default for Inflate {
    fn default() -> Self {
        Self::new()
    }
}

fn lengths_to_codes(lens: &[u8], codes: &mut [ShortCodebookDesc]) -> DecompressResult<()> {
    let mut bits = [0u32; 32];
    let mut pfx  = [0u32; 33];
    for len in lens.iter() {
        let len = *len as usize;
        if len >= bits.len() {
            return Err(DecompressError::InvalidHeader);
        }
        bits[len] += 1;
    }
    bits[0] = 0;
    let mut code = 0;
    for i in 0..bits.len() {
        code = (code + bits[i]) << 1;
        pfx[i + 1] = code;
    }

    for (len, codes) in lens.iter().zip(codes.iter_mut()) {
        let len = *len as usize;
        if len != 0 {
            let bits = len as u8;
            *codes = ShortCodebookDesc { code: reverse_bits(pfx[len], bits), bits };
            pfx[len] += 1;
        } else {
            *codes = ShortCodebookDesc { code: 0, bits: 0 };
        }
    }

    Ok(())
}

struct GzipCRC32 {
    tab: [u32; 256],
    crc: u32,
}

impl GzipCRC32 {
    fn new() -> Self {
        let mut tab = [0u32; 256];
        for (i, el) in tab.iter_mut().enumerate() {
            let mut c = i as u32;
            for _ in 0..8 {
                if (c & 1) != 0 {
                    c = 0xEDB88320 ^ (c >> 1);
                } else {
                    c >>= 1;
                }
            }
            *el = c;
        }
        Self { tab, crc: 0 }
    }
    fn update_crc(&mut self, src: &[u8]) {
        let mut c = !self.crc;
        for el in src.iter() {
            c = self.tab[((c ^ u32::from(*el)) & 0xFF) as usize] ^ (c >> 8);
        }
        self.crc = !c;
    }
}

/// Decodes input data in gzip file format (RFC 1952) returning a vector containing decoded data.
pub fn gzip_decode(br: &mut dyn ByteIO, skip_crc: bool) -> DecompressResult<Vec<u8>> {
    const FLAG_HCRC:    u8 = 0x02;
    const FLAG_EXTRA:   u8 = 0x04;
    const FLAG_NAME:    u8 = 0x08;
    const FLAG_COMMENT: u8 = 0x10;

    let id1 = br.read_byte()?;
    let id2 = br.read_byte()?;
    let cm  = br.read_byte()?;
    let flg = br.read_byte()?;
    let _mtime = br.read_u32le()?;
    let _xfl   = br.read_byte()?;
    let _os    = br.read_byte()?;
    if id1 != 0x1F || id2 != 0x8B || cm != 8 {
        return Err(DecompressError::InvalidHeader);
    }

    if (flg & FLAG_EXTRA) != 0 {
        let xlen = br.read_u16le()? as usize;
        br.read_skip(xlen)?;
    }
    if (flg & FLAG_NAME) != 0 {
        loop {
            let b = br.read_byte()?;
            if b == 0 {
                break;
            }
        }
    }
    if (flg & FLAG_COMMENT) != 0 {
        loop {
            let b = br.read_byte()?;
            if b == 0 {
                break;
            }
        }
    }
    let _hcrc =  if (flg & FLAG_HCRC) != 0 {
            br.read_u16le()?
        } else {
            0
        };
    if (flg & 0xE0) != 0 {
        return Err(DecompressError::Unsupported);
    }

    let mut output: Vec<u8> = Vec::new();
    let mut tail = [0u8; 8];
    let mut inblk = [0u8; 1024];
    let mut oblk = [0u8; 4096];
    let mut inflate = Inflate::new();
    let mut checker = GzipCRC32::new();

    loop {
        let ret = br.read_buf_some(&mut inblk);
        if let Err(ByteIOError::EOF) = ret {
            break;
        }
        let inlen = match ret {
                Ok(val) => val,
                Err(_)  => return Err(DecompressError::IOError),
            };
        let mut repeat = false;
        loop {
            let ret = inflate.decompress_data(&inblk[..inlen], &mut oblk, repeat);
            match ret {
                Ok(outlen) => {
                    checker.update_crc(&oblk[..outlen]);
                    output.extend_from_slice(&oblk[..outlen]);
                    break;
                },
                Err(DecompressError::ShortData) => {
                    break;
                },
                Err(DecompressError::OutputFull) => {
                    repeat = true;
                    checker.update_crc(&oblk);
                    output.extend_from_slice(&oblk);
                },
                Err(err) => {
                    return Err(err);
                },
            }
        }
        // Save last 8 bytes for CRC and size.
        if inlen >= 8 {
            tail.copy_from_slice(&inblk[inlen - 8..][..8]);
        } else {
            let shift_len = 8 - inlen;
            for i in 0..shift_len {
                tail[i] = tail[i + inlen];
            }
            tail[shift_len..8].copy_from_slice(&inblk[..8 - shift_len]);
        }
    }
    if !skip_crc {
        if !inflate.is_finished() { println!("???"); }
        let crc  = read_u32le(&tail[0..4])?;
        let size = read_u32le(&tail[4..8])?;
        if size != (output.len() as u32) {
            return Err(DecompressError::CRCError);
        }
        if crc != checker.crc {
            return Err(DecompressError::CRCError);
        }
    }

    Ok(output)
}

#[derive(Clone,Copy,Default)]
struct Token {
    sym:        u16,
    distsym:    u8,
    len:        u16,
    dist:       u16,
}

const TOKEN_EOB: Token = Token { sym: 256, distsym: 0, len: 0, dist: 0 };

impl Token {
    fn from_literal(sym: u8) -> Self {
        Self {
            sym:        u16::from(sym),
            distsym:    0,
            dist:       0,
            len:        0,
        }
    }
    fn from_match(dist: u16, len: u16) -> Self {
        let sym = match len {
              3..= 10 => 257 +  len -   3,
             11..= 18 => 265 + (len -  11) /  2,
             19..= 34 => 269 + (len -  19) /  4,
             35..= 66 => 273 + (len -  35) /  8,
             67..=130 => 277 + (len -  67) / 16,
            131..=257 => 281 + (len - 131) / 32,
                    _ => 285,
        };
        let distsym = if dist <= 4 {
                (dist - 1) as u8
            } else {
                let bits = 16 - (dist - 1).leading_zeros();
                (bits as u8) * 2 - 2 + if ((dist - 1) & (1 << (bits - 2))) != 0 { 1 } else { 0 }
            };
        Self {
            sym, distsym, len, dist
        }
    }
}

fn add_codes(lens: &[u8], stats: &mut [u32], toks: &mut Vec<(u8, u8)>) {
    let mut last = 42;
    let mut lcount = 0;

    for &len in lens.iter() {
        if len == last {
            lcount += 1;
        } else {
            if last == 0 {
                while lcount > 10 {
                    let run = lcount.min(138);
                    stats[18] += 1;
                    toks.push((18, run - 11));
                    lcount -= run;
                }
                if lcount >= 3 {
                    stats[17] += 1;
                    toks.push((17, lcount - 3));
                    lcount = 0;
                }
                for _ in 0..lcount {
                    stats[0] += 1;
                    toks.push((0, 0));
                }
            } else {
                while lcount >= 3 {
                    let run = lcount.min(6);
                    stats[16] += 1;
                    toks.push((16, run - 3));
                    lcount -= run;
                }
                for _ in 0..lcount {
                    stats[last as usize] += 1;
                    toks.push((last, 0));
                }
            }
            stats[len as usize] += 1;
            toks.push((len, 0));
            last = len;
            lcount = 0;
        }
    }
    if lcount > 0 {
        if last == 0 {
            while lcount > 10 {
                let run = lcount.min(138);
                stats[18] += 1;
                toks.push((18, run - 11));
                lcount -= run;
            }
            if lcount >= 3 {
                stats[17] += 1;
                toks.push((17, lcount - 3));
                lcount = 0;
            }
            for _ in 0..lcount {
                stats[0] += 1;
                toks.push((0, 0));
            }
        } else {
            while lcount >= 3 {
                let run = lcount.min(6);
                stats[16] += 1;
                toks.push((16, run - 3));
                lcount -= run;
            }
            for _ in 0..lcount {
                stats[last as usize] += 1;
                toks.push((last, 0));
            }
        }
    }
}

/// Deflate stream writer.
pub struct DeflateWriter {
    dst:    Vec<u8>,
    bits:   u8,
    bbuf:   u32,
}

impl DeflateWriter {
    /// Creates a new instance of `DeflateWriter` for a provided output.
    pub fn new(dst: Vec<u8>) -> Self {
        Self {
            dst,
            bits:   0,
            bbuf:   0,
        }
    }
    fn align(&mut self) {
        if (self.bits & 7) != 0 {
            self.bits += 8 - (self.bits & 7);
        }
    }
    fn flush(&mut self) {
        while self.bits >= 8 {
            self.dst.push(self.bbuf as u8);
            self.bbuf >>= 8;
            self.bits -= 8;
        }
    }
    fn write(&mut self, val: u16, len: u8) {
        self.flush();
        self.bbuf |= u32::from(val) << self.bits;
        self.bits += len;
    }
    /// Finishes writing the stream and returns the output vector.
    pub fn end(mut self) -> Vec<u8> {
        self.flush();
        if self.bits > 0 {
            self.dst.push(self.bbuf as u8);
        }
        self.dst
    }

    fn write_codes(&mut self, codes: &CodeHuff, dists: &DistHuff) {
        let mut stats = [0u32; 19];
        let mut toks = Vec::with_capacity(NUM_LITERALS + NUM_DISTS);
        let mut cw = [0u16; 19];
        let mut cl = [0u8; 19];
        let mut nc = 0;

        add_codes(&codes.lens[..codes.num_codes], &mut stats, &mut toks);
        add_codes(&dists.lens[..dists.num_codes], &mut stats, &mut toks);

        gen_tree(&mut cw, &mut cl, &mut nc, &mut stats, 7);

        nc = cw.len();
        for &idx in LEN_RECODE.iter().rev() {
            if cl[idx] == 0 {
                nc -= 1;
            } else {
                break;
            }
        }
        if nc < 4 {
            nc = 4;
        }
        self.write((nc - 4) as u16, 4);
        for &idx in LEN_RECODE.iter().take(nc) {
            self.write(u16::from(cl[idx]), 3);
        }
        for &(sym, add) in toks.iter() {
            self.write(cw[sym as usize], cl[sym as usize]);
            match sym {
                16 => self.write(u16::from(add), 2),
                17 => self.write(u16::from(add), 3),
                18 => self.write(u16::from(add), 7),
                _  => {},
            };
        }
    }
    fn write_tokens(&mut self, src: &[Token], codes: &CodeHuff, dists: &DistHuff) {
        for &tok in src.iter() {
            self.write(codes.codes[tok.sym as usize], codes.lens[tok.sym as usize]);
            if tok.sym > 256 {
                self.write_len_bits(tok.len);
                self.write(dists.codes[tok.distsym as usize], dists.lens[tok.distsym as usize]);
                self.write_dist_bits(tok.dist);
            }
        }
    }
    fn write_len_bits(&mut self, len: u16) {
        let llen = len - 3;
        if (8..255).contains(&llen) {
            let bits = (16 - llen.leading_zeros() - 3) as u8;
            self.write(llen & ((1 << bits) - 1), bits);
        }
    }
    fn write_dist_bits(&mut self, dist: u16) {
        let ddist = dist - 1;
        if dist >= 4 {
            let bits = (16 - ddist.leading_zeros() - 2) as u8;
            self.write(ddist & ((1 << bits) - 1), bits);
        }
    }
}

struct CodeHuff {
    is_fixed:   bool,
    stats:      [u32; NUM_LITERALS],
    codes:      [u16; NUM_LITERALS],
    lens:       [u8;  NUM_LITERALS],
    num_codes:  usize,
}

impl CodeHuff {
    fn new(is_fixed: bool) -> Self {
        Self {
            is_fixed,
            stats:      [0; NUM_LITERALS],
            codes:      [0; NUM_LITERALS],
            lens:       [0; NUM_LITERALS],
            num_codes:  NUM_LITERALS,
        }
    }
    fn make_codes(&mut self, src: &[Token]) {
        if self.is_fixed {
            for i in 0..=143 {
                self.codes[i] = reverse_bits((i + 0x30) as u32, 8) as u16;
                self.lens[i]  = 8;
            }
            for i in 144..=255 {
                self.codes[i] = reverse_bits((i + 0x100) as u32, 9) as u16;
                self.lens[i]  = 9;
            }
            for i in 256..=279 {
                self.codes[i] = reverse_bits((i & 0x1F) as u32, 7) as u16;
                self.lens[i]  = 7;
            }
            for i in 280..NUM_LITERALS {
                self.codes[i] = reverse_bits((i - 280 + 0xC0) as u32, 8) as u16;
                self.lens[i]  = 8;
            }
        } else {
            for &tok in src.iter() {
                self.stats[tok.sym as usize] += 1;
            }
            gen_tree(&mut self.codes, &mut self.lens, &mut self.num_codes, &mut self.stats, 15);
            if self.num_codes < 257 {
                self.num_codes = 257;
            }
        }
    }
}

struct DistHuff {
    is_fixed:   bool,
    stats:      [u32; NUM_DISTS],
    codes:      [u16; NUM_DISTS],
    lens:       [u8;  NUM_DISTS],
    num_codes:  usize,
}

impl DistHuff {
    fn new(is_fixed: bool) -> Self {
        Self {
            is_fixed,
            stats:      [0; NUM_DISTS],
            codes:      [0; NUM_DISTS],
            lens:       [0; NUM_DISTS],
            num_codes:  NUM_DISTS,
        }
    }
    fn make_codes(&mut self, src: &[Token]) {
        if self.is_fixed {
            for i in 0..NUM_DISTS {
                self.codes[i] = reverse_bits(i as u32, 5) as u16;
                self.lens[i]  = 5;
            }
        } else {
            for &tok in src.iter() {
                if tok.sym > 256 {
                    self.stats[tok.distsym as usize] += 1;
                }
            }
            gen_tree(&mut self.codes, &mut self.lens, &mut self.num_codes, &mut self.stats, 15);
            if self.num_codes < 1 {
                self.num_codes = 1;
            }
        }
    }
}

#[derive(Clone,Copy,Default)]
struct Node {
    sym:    u16,
    w:      u16,
    idx0:   u16,
    idx1:   u16,
}

const NODE_SYM: u16 = 65500;

struct Tree {
    nodes:  [Node; NUM_LITERALS * 2],
    nnodes: usize,
}

impl Tree {
    fn new() -> Self {
        Self {
            nodes:  [Node::default(); NUM_LITERALS * 2],
            nnodes: 0,
        }
    }
    fn insert(&mut self, val: Node) {
        let mut idx = self.nnodes;
        for (i, nn) in self.nodes[..self.nnodes].iter().enumerate() {
            if nn.w > val.w {
                idx = i;
                break;
            }
        }
        if idx < self.nnodes {
            for i in (idx..self.nnodes).rev() {
                self.nodes[i + 1] = self.nodes[i];
            }
        }
        self.nodes[idx] = val;
        self.nnodes += 1;
    }
    fn trim(&mut self) {
        let mut start = self.nnodes;
        for (i, n) in self.nodes[..self.nnodes].iter().enumerate() {
            if n.w != 0 {
                start = i;
                break;
            }
        }
        if start != 0 {
            for i in 0..self.nnodes - start {
                self.nodes[i] = self.nodes[i + start];
            }
            self.nnodes -= start;
        }
    }
    fn build(&mut self) {
        if self.nnodes == 1 {
            self.nodes[0].w = 1;
            return;
        }
        let mut start = 0;
        while start + 1 < self.nnodes {
            let nn1 = self.nodes[start];
            let nn2 = self.nodes[start + 1];
            let n = Node {
                sym:    NODE_SYM,
                w:      nn1.w + nn2.w,
                idx0:   start as u16,
                idx1:   (start + 1) as u16,
            };
            self.nodes[start].w = 0;
            self.nodes[start + 1].w = 0;
            start += 2;
            self.insert(n);
        }
        if self.nnodes > 1 {
            self.assign_len(self.nnodes - 1, 0);
        }
    }
    fn assign_len(&mut self, idx: usize, len: u16) {
        if self.nodes[idx].sym == NODE_SYM {
            self.assign_len(self.nodes[idx].idx0 as usize, len + 1);
            self.assign_len(self.nodes[idx].idx1 as usize, len + 1);
        } else {
            self.nodes[idx].w = len;
        }
    }
}

fn gen_tree(codes: &mut [u16], lens: &mut [u8], num_codes: &mut usize, stats: &mut [u32], max_bits: u8) {
    let mut tot_w = 0;
    for &w in stats.iter() {
        tot_w += w;
    }
    if tot_w == 0 {
        codes[0] = 0;
        lens[0] = 0;
        *num_codes = 0;
        return;
    }
    loop {
        let mut tree = Tree::new();
        for (sym, &w) in stats.iter().enumerate() {
            tree.insert(Node{ sym: sym as u16, w: w as u16, idx0: 64000, idx1: 64000 });
        }
        tree.trim();
        tree.build();

        for n in tree.nodes[..tree.nnodes].iter() {
            if n.sym != NODE_SYM {
                lens[n.sym as usize] = n.w as u8;
            }
        }
        if !lens.iter().any(|&x| x > max_bits) {
            break;
        } else {
            for w in stats.iter_mut() {
                *w = (*w + 1) >> 1;
            }
        }
    }
    lengths_to_codes16(lens, codes);
    let mut sz = codes.len();
    for &len in lens.iter().rev() {
        if len != 0 {
            break;
        }
        sz -= 1;
    }
    *num_codes = sz;
}

fn lengths_to_codes16(lens: &[u8], codes: &mut [u16]) {
    let mut bits = [0u32; 32];
    let mut pfx  = [0u32; 33];
    for len in lens.iter() {
        let len = *len as usize;
        bits[len] += 1;
    }
    bits[0] = 0;
    let mut code = 0;
    for i in 0..bits.len() {
        code = (code + bits[i]) << 1;
        pfx[i + 1] = code;
    }

    for (len, codes) in lens.iter().zip(codes.iter_mut()) {
        let len = *len as usize;
        if len != 0 {
            let bits = len as u8;
            *codes = reverse_bits(pfx[len], bits) as u16;
            pfx[len] += 1;
        } else {
            *codes = 0;
        }
    }
}

trait LZParse {
    fn parse(&mut self, src: &[u8], dst: &mut Vec<Token>);
}

struct NoParser {}
impl LZParse for NoParser {
    fn parse(&mut self, src: &[u8], dst: &mut Vec<Token>) {
        dst.reserve(src.len());
        for &b in src.iter() {
            dst.push(Token::from_literal(b));
        }
        dst.push(TOKEN_EOB);
    }
}

fn check_match(src1: &[u8], src2: &[u8]) -> u16 {
    let mut len = 0;
    for (&a, &b) in src1.iter().zip(src2.iter()) {
        if a != b {
            break;
        }
        len += 1;
    }
    len
}

const HASH_SIZE: usize = 4096;
const MAX_MATCH_LEN: usize = 258;
const WINDOW_SIZE: usize = 32768 - MAX_MATCH_LEN;
const WINDOW_MASK: usize = 32767;

struct MatchFinder<'a> {
    src:        &'a [u8],
    pos:        usize,
    hstart:     [usize; HASH_SIZE],
    hnext:      [usize; WINDOW_MASK + 1],
}

impl<'a> MatchFinder<'a> {
    fn new(src: &'a [u8]) -> Self {
        Self {
            src,
            pos:        0,
            hstart:     [src.len(); HASH_SIZE],
            hnext:      [src.len(); WINDOW_MASK + 1],
        }
    }
    fn hash(src: &[u8]) -> usize {
        let _ = src[2];
        (((u16::from(src[0]) << 10) ^ (u16::from(src[1]) << 5) ^ u16::from(src[2])) & ((HASH_SIZE as u16) - 1)) as usize
    }
    fn add_hash(&mut self, hash: usize) {
        self.hnext[self.pos & WINDOW_MASK] = self.hstart[hash];
        self.hstart[hash] = self.pos;
        self.hnext[(self.pos + 1) & WINDOW_MASK] = self.src.len();
    }
    fn find_match(&mut self) -> (u16, u16) {
        if self.pos == 0 || self.pos + 3 > self.src.len() {
            return (0, 0);
        }
        let key = Self::hash(&self.src[self.pos..]);

        let mut best_pos = 0;
        let mut best_len = 0;
        let mut idx = self.hstart[key];
        let search_end = self.pos.saturating_sub(WINDOW_SIZE);
        let mut tries = 0;
        while idx >= search_end && idx < self.pos {
            let cur_len = check_match(&self.src[self.pos..], &self.src[idx..]);
            if cur_len > best_len {
                best_len = cur_len;
                best_pos = self.pos - idx;
                if best_len >= (MAX_MATCH_LEN as u16) {
                    return (best_pos as u16, MAX_MATCH_LEN as u16);
                }
            }
            tries += 1;
            if tries > 16 {
                break;
            }
            idx = self.hnext[idx & WINDOW_MASK];
        }
        self.add_hash(key);
        (best_pos as u16, best_len)
    }
    fn find_all_matches(&mut self, dists: &mut [u16; MAX_MATCH_LEN + 1]) {
        if self.pos == 0 || self.pos + 3 > self.src.len() {
            return;
        }
        let key = Self::hash(&self.src[self.pos..]);
        let mut idx = self.hstart[key];
        let search_end = self.pos.saturating_sub(WINDOW_SIZE);
        let mut tries = 0;
        while idx >= search_end && idx < self.pos {
            let cur_len = (check_match(&self.src[self.pos..], &self.src[idx..]) as usize).min(MAX_MATCH_LEN);
            if cur_len > 0 && dists[cur_len] == 0 {
                dists[cur_len] = (self.pos - idx) as u16;
            }
            idx = self.hnext[idx & WINDOW_MASK];
            tries += 1;
            if tries > 128 {
                break;
            }
        }
        self.add_hash(key);
    }
    fn advance(&mut self, num: usize) {
        self.pos += 1;
        if num > 1 {
            for _ in 1..num {
                if self.pos + 3 <= self.src.len() {
                    let key = Self::hash(&self.src[self.pos..]);
                    self.add_hash(key);
                }
                self.pos += 1;
            }
        }
    }
    fn get_sym(&self) -> u8 { self.src[self.pos] }
    fn is_end(&self) -> bool { self.pos >= self.src.len() }
}

struct GreedyParser {}
impl LZParse for GreedyParser {
    fn parse(&mut self, src: &[u8], dst: &mut Vec<Token>) {
        dst.reserve(src.len());

        let mut matcher = MatchFinder::new(src);
        while !matcher.is_end() {
            let (best_pos, best_len) = matcher.find_match();

            if best_len >= 3 {
                dst.push(Token::from_match(best_pos, best_len));
                matcher.advance(best_len as usize);
            } else {
                dst.push(Token::from_literal(matcher.get_sym()));
                matcher.advance(1);
            }
        }
        dst.push(TOKEN_EOB);
    }
}

struct LazyParser {}
impl LZParse for LazyParser {
    fn parse(&mut self, src: &[u8], dst: &mut Vec<Token>) {
        dst.reserve(src.len());

        let mut matcher = MatchFinder::new(src);
        while !matcher.is_end() {
            let (best_pos, best_len) = matcher.find_match();
            if best_len >= 3 {
                let last_sym = matcher.get_sym();
                matcher.advance(1);
                if !matcher.is_end() {
                    let (best_pos1, best_len1) = matcher.find_match();
                    if best_len1 > best_len + 1 {
                        dst.push(Token::from_literal(last_sym));
                        dst.push(Token::from_match(best_pos1, best_len1));
                        matcher.advance(best_len1 as usize);
                        continue;
                    }
                }
                dst.push(Token::from_match(best_pos, best_len));
                matcher.advance((best_len - 1) as usize);
            } else {
                dst.push(Token::from_literal(matcher.get_sym()));
                matcher.advance(1);
            }
        }
        dst.push(TOKEN_EOB);
    }
}

#[derive(Clone,Copy)]
struct TNode {
    price:      u32,
    dist:       u16,
    link:       usize,
}

impl Default for TNode {
    fn default() -> Self {
        Self {
            price:  u32::MAX,
            dist:   0,
            link:   0,
        }
    }
}

struct OptimalParser {
    trellis:    Vec<TNode>,
}
impl OptimalParser {
    fn new() -> Self { Self::default() }
    fn sym_price(_sym: u8) -> u32 { 9 }
    fn match_price(dist: u16, _len: u16) -> u32 {
        if dist <= 4 {
            9 + 5
        } else {
            let bits = 16 - (dist - 1).leading_zeros();
            9 + 3 + bits
        }
    }
}
impl Default for OptimalParser {
    fn default() -> Self {
        Self {
            trellis: Vec::with_capacity(WINDOW_SIZE),
        }
    }
}
impl LZParse for OptimalParser {
    fn parse(&mut self, src: &[u8], dst: &mut Vec<Token>) {
        if src.is_empty() {
            dst.push(TOKEN_EOB);
            return;
        }
        dst.reserve(src.len());

        self.trellis.clear();
        self.trellis.reserve(src.len() + 1);
        for _ in 0..=src.len() {
            self.trellis.push(TNode::default());
        }
        self.trellis[0].price = 0;

        let mut matcher = MatchFinder::new(src);
        for i in 0..self.trellis.len() - 1 {
            let mut dists = [0; MAX_MATCH_LEN + 1];
            matcher.find_all_matches(&mut dists);

            let sym = matcher.get_sym();
            let lprice = Self::sym_price(sym) + self.trellis[i].price;
            if self.trellis[i + 1].price > lprice {
                self.trellis[i + 1].price = lprice;
                self.trellis[i + 1].link = i;
            }
            for (len, &dist) in dists.iter().enumerate() {
                if dist != 0 {
                    let mprice = Self::match_price(dist, len as u16) + self.trellis[i].price;
                    if self.trellis[i + len].price > mprice {
                        self.trellis[i + len].price = mprice;
                        self.trellis[i + len].link = i;
                        self.trellis[i].dist = dist;
                    }
                }
            }

            matcher.advance(1);
        }
        let mut idx = self.trellis.len() - 1;
        let mut nidx = self.trellis[idx].link;
        while idx > 0 {
            let oidx = idx;
            idx = nidx;
            nidx = self.trellis[idx].link;
            self.trellis[idx].link = oidx;
        }

        let mut idx = 0;
        while idx < self.trellis.len() - 1 {
            let len = self.trellis[idx].link - idx;
            if len == 1 {
                dst.push(Token::from_literal(src[idx]));
            } else {
                dst.push(Token::from_match(self.trellis[idx].dist, len as u16));
            }
            idx = self.trellis[idx].link;
        }

        dst.push(TOKEN_EOB);
    }
}

/// Deflate compression mode.
#[derive(Clone,Copy,Debug,PartialEq,Default)]
pub enum DeflateMode {
    /// No compression.
    NoCompr,
    /// Fast compression.
    Fast,
    /// Still fast but better compression.
    #[default]
    Better,
    /// Slow but the best compression.
    Best,
}

pub const DEFLATE_MODE_DESCRIPTION: &str = "Deflate compression level.";
/// Deflate option for no compression.
pub const DEFLATE_MODE_NONE: &str = "none";
/// Deflate option for fast compression.
pub const DEFLATE_MODE_FAST: &str = "fast";
/// Deflate option for better compression.
pub const DEFLATE_MODE_BETTER: &str = "better";
/// Deflate option for best compression.
pub const DEFLATE_MODE_BEST: &str = "best";

/// All possible option values for deflate compression.
pub const DEFLATE_OPTION_VALUES: NAOptionDefinitionType = NAOptionDefinitionType::String(Some(&[DEFLATE_MODE_NONE, DEFLATE_MODE_FAST, DEFLATE_MODE_BETTER, DEFLATE_MODE_BEST]));

impl std::str::FromStr for DeflateMode {
    type Err = ();

    fn from_str(s: &str) -> Result<Self, Self::Err> {
        match s {
            DEFLATE_MODE_NONE   => Ok(DeflateMode::NoCompr),
            DEFLATE_MODE_FAST   => Ok(DeflateMode::Fast),
            DEFLATE_MODE_BETTER => Ok(DeflateMode::Better),
            DEFLATE_MODE_BEST   => Ok(DeflateMode::Best),
            _ => Err(()),
        }
    }
}

impl std::fmt::Display for DeflateMode {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> Result<(), std::fmt::Error> {
        match *self {
            DeflateMode::NoCompr    => write!(f, "{DEFLATE_MODE_NONE}"),
            DeflateMode::Fast       => write!(f, "{DEFLATE_MODE_FAST}"),
            DeflateMode::Better     => write!(f, "{DEFLATE_MODE_BETTER}"),
            DeflateMode::Best       => write!(f, "{DEFLATE_MODE_BEST}"),
        }
    }
}

#[derive(Clone,Copy,Debug,PartialEq)]
enum Mode {
    Copy,
    Fixed,
    Dynamic,
}

const MAX_BLOCK_SIZE: usize = 65535;

/// Deflate stream compressor.
pub struct Deflate {
    mode:       Mode,
    tokens:     Vec<Token>,
    srcbuf:     [u8; MAX_BLOCK_SIZE],
    ssize:      usize,
    sum1:       u32,
    sum2:       u32,
    zlib_mode:  bool,
    parser:     Box<dyn LZParse + Send>,
}

impl Deflate {
    /// Creates a new instance of `Deflate`.
    pub fn new(mode: DeflateMode) -> Self {
        let (mode, parser) = match mode {
            DeflateMode::NoCompr => (Mode::Copy,    Box::new(NoParser{}) as Box<dyn LZParse + Send>),
            DeflateMode::Fast    => (Mode::Fixed,   Box::new(GreedyParser{}) as Box<dyn LZParse + Send>),
            DeflateMode::Better  => (Mode::Dynamic, Box::new(LazyParser{}) as Box<dyn LZParse + Send>),
            DeflateMode::Best    => (Mode::Dynamic, Box::new(OptimalParser::new()) as Box<dyn LZParse + Send>),
        };
        Self {
            mode, parser,
            tokens:     Vec::with_capacity(MAX_BLOCK_SIZE),
            srcbuf:     [0; MAX_BLOCK_SIZE],
            ssize:      0,
            sum1:       1,
            sum2:       0,
            zlib_mode:  false,
        }
    }
    /// Writes zlib stream header.
    pub fn write_zlib_header(&mut self, wr: &mut DeflateWriter) {
        wr.write(8, 4);
        wr.write(7, 4);
        let level = match self.mode {
            Mode::Copy      => 0x01,
            Mode::Fixed     => 0x5E,
            Mode::Dynamic   => 0x9C,
            // 0xDA for the strongest one
            };
        wr.write(level, 8);
        self.zlib_mode = true;
    }
    fn write_zlib_footer(&self, wr: &mut DeflateWriter) {
        wr.align();
        wr.write((self.sum2 >> 8)   as u16, 8);
        wr.write((self.sum2 & 0xFF) as u16, 8);
        wr.write((self.sum1 >> 8)   as u16, 8);
        wr.write((self.sum1 & 0xFF) as u16, 8);
    }
    /// Queues data for compression.
    ///
    /// The data might be not actually compressed until [`compress_end`] is called.
    ///
    /// [`compress_end`]: ./struct.Deflate.html#method.compress_end
    pub fn compress(&mut self, src: &[u8], wr: &mut DeflateWriter) {
        let mut src = src;
        while !src.is_empty() {
            let clen = src.len().min(MAX_BLOCK_SIZE - self.ssize);
            let (head, tail) = src.split_at(clen);
            src = tail;
            self.srcbuf[self.ssize..][..clen].copy_from_slice(head);
            self.ssize += clen;
            if self.ssize == MAX_BLOCK_SIZE {
                self.do_block(wr, false);
            }
        }
    }
    /// Tells the encoder to finish data compression.
    ///
    /// Complete data will be output after this call.
    pub fn compress_end(&mut self, wr: &mut DeflateWriter) {
        if self.ssize > 0 {
            self.do_block(wr, true);
        } else {
            wr.write(1, 1);
            wr.write(1, 2);
            wr.write(0, 7); //static EOF sym
        }
        if self.zlib_mode {
            self.write_zlib_footer(wr);
        }
    }
    /// Tells the encoder to compress the data it received and flush it.
    pub fn compress_flush(&mut self, wr: &mut DeflateWriter) {
        if self.ssize > 0 {
            self.do_block(wr, false);
        }
        if (wr.bits & 7) != 0 {
            // write zero-length copy block for byte-alignment
            wr.write(0, 1);
            wr.write(0, 2);
            wr.align();
            wr.write(0, 16);
            wr.write(0xFFFF, 16);
        }
    }
    fn do_block(&mut self, wr: &mut DeflateWriter, final_block: bool) {
        const CRC_BASE: u32 = 65521;
        for &b in self.srcbuf[..self.ssize].iter() {
            self.sum1 += u32::from(b);
            if self.sum1 >= CRC_BASE {
                self.sum1 -= CRC_BASE;
            }
            self.sum2 += self.sum1;
            if self.sum2 >= CRC_BASE {
                self.sum2 -= CRC_BASE;
            }
        }
        match self.mode {
            Mode::Copy => {
                wr.write(final_block as u16, 1);
                wr.write(0, 2);
                wr.align();
                wr.write(self.ssize as u16, 16);
                wr.write(!self.ssize as u16, 16);
                for &b in self.srcbuf[..self.ssize].iter() {
                    wr.write(u16::from(b), 8);
                }
            },
            Mode::Fixed => {
                wr.write(final_block as u16, 1);
                wr.write(1, 2);
                self.tokens.clear();
                self.parser.parse(&self.srcbuf[..self.ssize], &mut self.tokens);
                let mut codes = CodeHuff::new(true);
                codes.make_codes(&self.tokens);
                let mut dists = DistHuff::new(true);
                dists.make_codes(&self.tokens);
                wr.write_tokens(&self.tokens, &codes, &dists);
            },
            Mode::Dynamic => {
                wr.write(final_block as u16, 1);
                wr.write(2, 2);
                self.tokens.clear();
                self.parser.parse(&self.srcbuf[..self.ssize], &mut self.tokens);
                let mut codes = CodeHuff::new(false);
                codes.make_codes(&self.tokens);
                let mut dists = DistHuff::new(false);
                dists.make_codes(&self.tokens);
                wr.write((codes.num_codes - 257) as u16, 5);
                wr.write((dists.num_codes - 1) as u16, 5);
                wr.write_codes(&codes, &dists);
                wr.write_tokens(&self.tokens, &codes, &dists);
            },
        }
        self.ssize = 0;
    }
}

#[cfg(test)]
mod test {
    use super::*;

    #[test]
    fn test_inflate1() {
        const TEST_DATA: &[u8] = &[
                0xF3, 0x48, 0xCD, 0xC9, 0xC9, 0xD7, 0x51, 0x28,
                0xCF, 0x2F, 0xCA, 0x49, 0x51, 0x04, 0x00 ];
        const TEST_REF: &[u8] = b"Hello, world!";
        let mut dst_buf = [0u8; 13];
        let len = Inflate::uncompress(TEST_DATA, &mut dst_buf).unwrap();
        assert_eq!(len, 13);
        for i in 0..len {
            assert_eq!(dst_buf[i], TEST_REF[i]);
        }
    }
    #[test]
    fn test_inflate2() {
        const TEST_DATA3: &[u8] = &[ 0x4B, 0x4C, 0x44, 0x80, 0x24, 0x54, 0x80, 0x2C, 0x06, 0x00 ];
        const TEST_REF3: &[u8] = b"aaaaaaaaaaaabbbbbbbbbbbbbbbaaaaabbbbbbb";
        let mut dst_buf = [0u8; 39];

        let mut inflate = Inflate::new();
        let mut output_chunk = [0u8; 7];
        let mut output_pos = 0;
        for input in TEST_DATA3.chunks(3) {
            let mut repeat = false;
            loop {
                let ret = inflate.decompress_data(input, &mut output_chunk, repeat);
                match ret {
                    Ok(len) => {
                        for i in 0..len {
                            dst_buf[output_pos + i] = output_chunk[i];
                        }
                        output_pos += len;
                        break;
                    },
                    Err(DecompressError::ShortData) => {
                        break;
                    },
                    Err(DecompressError::OutputFull) => {
                        repeat = true;
                        for i in 0..output_chunk.len() {
                            dst_buf[output_pos + i] = output_chunk[i];
                        }
                        output_pos += output_chunk.len();
                    },
                    _ => {
                        panic!("decompress error {:?}", ret.err().unwrap());
                    },
                }
            }
        }

        assert_eq!(output_pos, dst_buf.len());
        for i in 0..output_pos {
            assert_eq!(dst_buf[i], TEST_REF3[i]);
        }
    }
    #[test]
    fn test_inflate3() {
        const TEST_DATA: &[u8] = &[
    0x1F, 0x8B, 0x08, 0x08, 0xF6, 0x7B, 0x90, 0x5E, 0x02, 0x03, 0x31, 0x2E, 0x74, 0x78, 0x74, 0x00,
    0xE5, 0x95, 0x4B, 0x4E, 0xC3, 0x30, 0x10, 0x40, 0xF7, 0x39, 0xC5, 0x1C, 0x00, 0x16, 0x70, 0x83,
    0x0A, 0xB5, 0x3B, 0xE8, 0x82, 0x5E, 0x60, 0x1A, 0x4F, 0xE2, 0x11, 0xFE, 0x44, 0x1E, 0xA7, 0x69,
    0x6E, 0xCF, 0x38, 0xDD, 0xB0, 0x40, 0xA2, 0x46, 0x2D, 0x20, 0x2A, 0xE5, 0xAB, 0xCC, 0xE7, 0xBD,
    0x49, 0xAC, 0x6C, 0x03, 0x64, 0x4B, 0xD0, 0x71, 0x92, 0x0C, 0x06, 0x67, 0x88, 0x1D, 0x3C, 0xD9,
    0xC4, 0x92, 0x3D, 0x4A, 0xF3, 0x3C, 0x43, 0x4E, 0x23, 0x81, 0x8B, 0x07, 0x82, 0x1E, 0xF5, 0x90,
    0x23, 0x78, 0x6A, 0x56, 0x30, 0x60, 0xCA, 0x89, 0x4D, 0x4F, 0xC0, 0x01, 0x10, 0x06, 0xC2, 0xA4,
    0xA1, 0x44, 0xCD, 0xF6, 0x54, 0x50, 0xA8, 0x8D, 0xC1, 0x9C, 0x5F, 0x71, 0x37, 0x45, 0xC8, 0x63,
    0xCA, 0x8E, 0xC0, 0xE8, 0x23, 0x69, 0x56, 0x9A, 0x8D, 0x5F, 0xB6, 0xC9, 0x96, 0x53, 0x4D, 0x17,
    0xAB, 0xB9, 0xB0, 0x49, 0x14, 0x5A, 0x0B, 0x96, 0x82, 0x7C, 0xB7, 0x6F, 0x17, 0x35, 0xC7, 0x9E,
    0xDF, 0x78, 0xA3, 0xF1, 0xD0, 0xA2, 0x73, 0x1C, 0x7A, 0xD8, 0x2B, 0xB3, 0x5C, 0x90, 0x85, 0xBB,
    0x2A, 0x14, 0x2E, 0xF7, 0xD1, 0x19, 0x48, 0x0A, 0x23, 0x57, 0x45, 0x13, 0x3E, 0xD6, 0xA0, 0xBD,
    0xF2, 0x11, 0x7A, 0x22, 0x21, 0xAD, 0xE5, 0x70, 0x56, 0xA0, 0x9F, 0xA5, 0xA5, 0x03, 0x85, 0x2A,
    0xDE, 0x92, 0x00, 0x32, 0x61, 0x10, 0xAD, 0x27, 0x13, 0x7B, 0x5F, 0x98, 0x7F, 0x59, 0x83, 0xB8,
    0xB7, 0x35, 0x16, 0xEB, 0x12, 0x0F, 0x1E, 0xD9, 0x14, 0x0B, 0xCF, 0xEE, 0x6D, 0x91, 0xF8, 0x93,
    0x6E, 0x81, 0x3F, 0x7F, 0x41, 0xA4, 0x22, 0x1F, 0xB7, 0xE6, 0x85, 0x83, 0x9A, 0xA2, 0x61, 0x12,
    0x0D, 0x0F, 0x6D, 0x01, 0xBD, 0xB0, 0xE8, 0x1D, 0xEC, 0xD1, 0xA0, 0xBF, 0x1F, 0x4E, 0xFB, 0x55,
    0xBD, 0x73, 0xDD, 0x87, 0xB9, 0x53, 0x23, 0x17, 0xD3, 0xE2, 0xE9, 0x08, 0x87, 0x42, 0xFF, 0xCF,
    0x26, 0x42, 0xAE, 0x76, 0xB5, 0xAE, 0x97, 0x0C, 0x18, 0x78, 0xA0, 0x24, 0xE5, 0x54, 0x0C, 0x6E,
    0x60, 0x52, 0x79, 0x22, 0x57, 0xF5, 0x87, 0x78, 0x78, 0x04, 0x93, 0x46, 0xEF, 0xCB, 0x98, 0x96,
    0x8B, 0x65, 0x00, 0xB7, 0x36, 0xBD, 0x77, 0xA8, 0xBD, 0x5A, 0xAA, 0x1A, 0x09, 0x00, 0x00
        ];

        let mut br = MemoryReader::new_read(TEST_DATA);
        let _dst_buf = gzip_decode(&mut br, false).unwrap();

//        println!("{}", String::from_utf8_lossy(_dst_buf.as_slice()));
    }
    #[test]
    fn test_deflate_crc() {
        let output = Vec::with_capacity(20);
        let mut writer = DeflateWriter::new(output);
        let mut compr = Deflate::new(DeflateMode::NoCompr);
        compr.write_zlib_header(&mut writer);
        compr.compress(b"Hello, world!", &mut writer);
        compr.compress_end(&mut writer);
        let output = writer.end();
        assert_eq!(output.as_slice(), b"\x78\x01\x01\x0D\x00\xF2\xFFHello, world!\x20\x5E\x04\x8A");
    }
    fn deflate_test(mode: DeflateMode) {
        const SRC: &[u8] =
b"The first day of Christmas,
My true love sent to me
A partridge in a pear tree.

The second day of Christmas,
My true love sent to me
Two turtle doves, and
A partridge in a pear tree.

The third day of Christmas,
My true love sent to me
Three French hens,
Two turtle doves, and
A partridge in a pear tree.

The fourth day of Christmas,
My true love sent to me
Four colly birds,
Three French hens,
Two turtle doves, and
A partridge in a pear tree.

The fifth day of Christmas,
My true love sent to me
Five gold rings,
Four colly birds,
Three French hens,
Two turtle doves, and
A partridge in a pear tree.";
        let output = Vec::with_capacity(SRC.len() + 16);
        let mut writer = DeflateWriter::new(output);
        let mut compr = Deflate::new(mode);
        compr.write_zlib_header(&mut writer);
        compr.compress(SRC, &mut writer);
        compr.compress_end(&mut writer);
        let output = writer.end();
        let mut uncompr = vec![0u8; SRC.len()];
        Inflate::uncompress(&output, &mut uncompr).unwrap();
        assert_eq!(SRC, uncompr.as_slice());
    }
    #[test]
    fn test_deflate_fast() {
        deflate_test(DeflateMode::Fast);
    }
    #[test]
    fn test_deflate_better() {
        deflate_test(DeflateMode::Better);
    }
    #[test]
    fn test_deflate_best() {
        deflate_test(DeflateMode::Best);
    }
}
