//! Audio output in WAV format.
use nihav_core::io::byteio::*;
use nihav_core::frame::*;
use std::io::SeekFrom;

/// WAVE output writer.
pub struct WavWriter<'a> {
    io: &'a mut dyn ByteIO,
    data_pos: u64,
}

fn write_byte(wr: &mut dyn ByteIO, sample: u8) -> ByteIOResult<()> {
    wr.write_byte(sample)
}

fn write_s16(wr: &mut dyn ByteIO, sample: i16) -> ByteIOResult<()> {
    wr.write_u16le(sample as u16)
}

fn write_s32(wr: &mut dyn ByteIO, sample: i32) -> ByteIOResult<()> {
    wr.write_u16le((sample >> 16) as u16)
}

fn write_f32(wr: &mut dyn ByteIO, sample: f32) -> ByteIOResult<()> {
    let mut out = ((sample * 32768.0) as i32).clamp(-32768, 32767);
    if out < 0 { out += 65536; }
    wr.write_u16le(out as u16)
}

macro_rules! write_data {
    ($wr:expr, $buf:expr, $write:ident) => ({
        let len = $buf.get_length();
        let ainfo = $buf.get_info();
        let nch = ainfo.get_channels() as usize;
        let mut offs: Vec<usize> = Vec::with_capacity(nch);
        for ch in 0..nch { offs.push($buf.get_offset(ch)); }
        let is_planar = $buf.get_step() == 1;
        let data = $buf.get_data();

        if is_planar {
            for i in 0..len {
                for ch in 0..nch {
                    let sample = data[offs[ch] + i];
                    $write($wr, sample)?;
                }
            }
        } else {
            for i in 0..len*nch {
                let sample = data[i];
                $write($wr, sample)?;
            }
        }
    })
}

impl<'a> WavWriter<'a> {
    /// Constructs a new `WavWriter` instance.
    pub fn new(io: &'a mut dyn ByteIO) -> Self {
        WavWriter { io, data_pos: 0 }
    }
    /// Writes audio format information to the file header.
    ///
    /// This function should be called exactly once before writing actual audio data.
    pub fn write_header(&mut self, ainfo: NAAudioInfo) -> ByteIOResult<()> {
        let bits = ainfo.get_format().get_bits() as usize;

        self.io.write_buf(b"RIFF")?;
        self.io.write_u32le(0)?;
        self.io.write_buf(b"WAVE")?;

        self.io.write_buf(b"fmt ")?;
        self.io.write_u32le(16)?;
        self.io.write_u16le(0x0001)?; // PCM
        self.io.write_u16le(u16::from(ainfo.get_channels()))?;
        self.io.write_u32le(ainfo.get_sample_rate())?;

        if bits < 16 {
            self.io.write_u32le(u32::from(ainfo.get_channels()) * ainfo.get_sample_rate())?;
            self.io.write_u16le(u16::from(ainfo.get_channels()))?; // block align
            self.io.write_u16le(8)?;
        } else {
            self.io.write_u32le(2 * u32::from(ainfo.get_channels()) * ainfo.get_sample_rate())?;
            self.io.write_u16le(u16::from(2 * ainfo.get_channels()))?; // block align
            self.io.write_u16le(16)?;
        }

        self.io.write_buf(b"data")?;
        self.io.write_u32le(0)?;

        self.data_pos = self.io.tell();
        Ok(())
    }
    /// Writes audio data.
    pub fn write_frame(&mut self, abuf: NABufferType) -> ByteIOResult<()> {
        match abuf {
            NABufferType::AudioU8(ref buf) => {
                write_data!(self.io, buf, write_byte);
            }
            NABufferType::AudioI16(ref buf) => {
                write_data!(self.io, buf, write_s16);
            }
            NABufferType::AudioI32(ref buf) => {
                write_data!(self.io, buf, write_s32);
            }
            NABufferType::AudioF32(ref buf) => {
                write_data!(self.io, buf, write_f32);
            }
            NABufferType::AudioPacked(ref buf) => {
                self.io.write_buf(buf.get_data().as_slice())?;
            }
            _ => {},
        };
        Ok(())
    }
}

impl<'a> Drop for WavWriter<'a> {
    #[allow(unused_variables)]
    fn drop(&mut self) {
        let size = self.io.tell();
        if (self.data_pos > 0) && (size >= self.data_pos) {
            let res = self.io.seek(SeekFrom::Start(4));
            let res = self.io.write_u32le((size - 8) as u32);
            let res = self.io.seek(SeekFrom::Start(self.data_pos - 4));
            let res = self.io.write_u32le((size - self.data_pos) as u32);
        }
    }
}
