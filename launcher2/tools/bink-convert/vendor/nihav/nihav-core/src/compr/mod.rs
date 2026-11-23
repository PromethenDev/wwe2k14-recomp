//! Various compression formats support.
#[cfg(feature="deflate")]
pub mod deflate;

use crate::io::byteio::ByteIOError;
use crate::io::bitreader::BitReaderError;
use crate::io::codebook::CodebookError;

/// A list specifying general compression errors.
#[derive(Debug)]
pub enum DecompressError {
    /// Compressed stream header contains errors.
    InvalidHeader,
    /// Compressed stream checksum does not match unpacked contents.
    CRCError,
    /// Compressed stream contains a combination of bits that cannot be decoded.
    InvalidData,
    /// Compressed stream ended prematurely.
    ShortData,
    /// There is more data than what can fit output buffer.
    OutputFull,
    /// Provided function argument is invalid.
    InvalidArgument,
    /// Compressed stream contains features that cannot be decoded.
    Unsupported,
    /// Underlying input reader had a problem.
    IOError,
}

/// A specialised `Result` type for codebook operations.
pub type DecompressResult<T> = Result<T, DecompressError>;

impl From<ByteIOError> for DecompressError {
    fn from(e: ByteIOError) -> Self {
        match e {
            ByteIOError::EOF => DecompressError::ShortData,
            _ => DecompressError::IOError,
        }
    }
}

impl From<BitReaderError> for DecompressError {
    fn from(e: BitReaderError) -> Self {
        match e {
            BitReaderError::BitstreamEnd => DecompressError::ShortData,
            _ => DecompressError::InvalidData,
        }
    }
}

impl From<CodebookError> for DecompressError {
    fn from(_: CodebookError) -> Self {
        DecompressError::InvalidData
    }
}

/// Copies requested amount of bytes from previous position in the same buffer.
/// If source area overlaps with destination area already copied values should be used e.g. copying with offset 1 means essentially to repeat previous byte requested number of times.
pub fn lz_copy(buf: &mut [u8], dst_pos: usize, offset: usize, len: usize) {
    if dst_pos < offset {
        panic!("Copy offset is before buffer start.");
    }
    let ipos = dst_pos - offset;
    let buf = &mut buf[ipos..];
    if ipos + len <= dst_pos {
        let (src, dst) = buf.split_at_mut(offset);
        dst[..len].copy_from_slice(&src[..len]);
    } else {
        for i in 0..len {
            buf[offset + i] = buf[i];
        }
    }
}
