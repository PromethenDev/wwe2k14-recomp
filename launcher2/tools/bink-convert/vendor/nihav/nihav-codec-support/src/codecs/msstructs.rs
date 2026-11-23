//! Common BITMAPINFOHEADER and WAVEFORMAT(EX) definitions and reading support.
use nihav_core::formats::*;
use nihav_core::frame::{NAVideoInfo, NAAudioInfo};
use nihav_core::io::byteio::*;

/// DIB/AVI bitmap header plus optional extradata
#[derive(Clone,Debug,Default)]
pub struct MSBitmapInfo {
    /// Original structure size.
    pub bi_size:        usize,
    /// Image width.
    pub width:          usize,
    /// Image height.
    pub height:         usize,
    /// Image is flipped (i.e. original height was negative).
    pub flipped:        bool,
    /// Number of image planes.
    pub planes:         u16,
    /// Bits per pixel.
    pub bitcount:       u16,
    /// Compression ID.
    pub compression:    [u8; 4],
    /// Original image size.
    pub image_size:     u32,
    /// Horizontal resolution.
    pub x_dpi:          u32,
    /// Vertical resolution,
    pub y_dpi:          u32,
    /// Number of colour indices in the palette.
    pub colors:         usize,
    /// Number of colours considered important for displaying bitmap.
    pub clr_important:  u32,
    /// Additional data.
    pub extradata:      Option<Vec<u8>>,
}

impl MSBitmapInfo {
    /// Checks header for validity and returns the failed field name in case of error.
    pub fn validate(&self) -> Result<(), &'static str> {
        const KNOWN_BAD_FORMATS: &[&[u8; 4]] = &[
            b"ZMBV", b"zmbv", b"CVID", b"cvid"
        ];
        if !(40..=(1 << 24)).contains(&self.bi_size) {
            return Err("bi_size");
        }
        if !(1..=65535).contains(&self.width) {
            return Err("width");
        }
        if !(1..=65535).contains(&self.height) {
            return Err("height");
        }
        if !KNOWN_BAD_FORMATS.contains(&&self.compression) {
            if (self.planes == 0 && self.bitcount <= 8) || self.planes > self.bitcount {
                return Err("planes");
            }
            if !(1..=32).contains(&self.bitcount) {
                return Err("bitcount");
            }
            if (self.bitcount <= 8 && !(&self.compression[..2] == b"Y8") && (self.colors == 0 || self.colors > (1 << self.bitcount))) || (self.bitcount > 8 && self.colors != 0) {
                return Err("clr_used");
            }
            if self.clr_important as usize > self.colors {
                return Err("clr_important");
            }
        }
        Ok(())
    }
    /// Reads structure from the input stream.
    ///
    /// Use `validate()` function for checking whether the result is valid.
    pub fn read(src: &mut dyn ByteIO) -> ByteIOResult<Self> {
        let bi_size         = src.read_u32le()? as usize;
        let width           = src.read_u32le()? as usize;
        let height          = src.read_u32le()? as i32;
        let flipped = height < 0;
        let height = height.unsigned_abs() as usize;
        let planes          = src.read_u16le()?;
        let bitcount        = src.read_u16le()?;
        let compression     = src.read_tag()?;
        let image_size      = src.read_u32le()?;
        let x_dpi           = src.read_u32le()?;
        let y_dpi           = src.read_u32le()?;
        let colors          = src.read_u32le()? as usize;
        let clr_important   = src.read_u32le()?;
        let extradata = if bi_size > 40 {
                let edata_size = bi_size - 40;
                if edata_size >= 1 << 24 {
                    return Err(ByteIOError::ReadError);
                }
                let mut edata = vec![0; edata_size];
                              src.read_buf(&mut edata)?;
                Some(edata)
            } else {
                None
            };
        Ok(Self {
            bi_size, width, height, flipped, planes, bitcount,
            compression, image_size, x_dpi, y_dpi, colors, clr_important,
            extradata
        })
    }
    /// Generates `NihAV` video information.
    pub fn get_video_info(&self) -> NAVideoInfo {
        let format = if self.bitcount > 8 { RGB24_FORMAT } else { PAL8_FORMAT };
        let mut vhdr = NAVideoInfo::new(self.width, self.height, self.flipped, format);
        vhdr.bits = (self.planes as u8) * (self.bitcount as u8);
        vhdr
    }
    /// Extracts extradata out of the structure and returns it.
    pub fn take_extradata(&mut self) -> Option<Vec<u8>> { self.extradata.take() }
    /// Reports number of bytes required to store this structure.
    pub fn get_size(&self) -> usize {
        40 + self.extradata.as_ref().map_or_else(|| 0, |edata| edata.len())
    }
    /// Writes structure to the provided output.
    pub fn write(&self, dst: &mut dyn ByteIO) -> ByteIOResult<()> {
        let size = self.get_size();
        dst.write_u32le(size as u32)?;
        dst.write_u32le(self.width as u32)?;
        if !self.flipped {
            dst.write_u32le(self.height as u32)?;
        } else {
            dst.write_u32le((-(self.height as i32)) as u32)?;
        }
        dst.write_u16le(self.planes)?;
        dst.write_u16le(self.bitcount)?;
        dst.write_buf(&self.compression)?;
        dst.write_u32le(self.image_size)?;
        dst.write_u32le(self.x_dpi)?;
        dst.write_u32le(self.y_dpi)?;
        dst.write_u32le(self.colors as u32)?;
        dst.write_u32le(self.clr_important)?;
        if let Some(ref edata) = self.extradata {
            dst.write_buf(edata.as_slice())?;
        }
        Ok(())
    }
}

/// Wave format definition.
#[derive(Clone,Debug,Default)]
pub struct MSWaveFormat {
    pub format_tag:         u16,
    pub channels:           u16,
    pub sample_rate:        u32,
    pub avg_bytes_per_sec:  u32,
    pub block_align:        usize,
    pub bits_per_sample:    u16,
    pub extradata:          Option<Vec<u8>>,
}

impl MSWaveFormat {
    /// Checks header for validity and returns the failed field name in case of error.
    pub fn validate(&self) -> Result<(), &'static str> {
        if !(1..=64).contains(&self.channels) {
            return Err("channels");
        }
        if !(4000..=400000).contains(&self.sample_rate) {
            return Err("sample_rate");
        }
        if self.block_align == 0 {
            return Err("block_align");
        }
        Ok(())
    }
    /// Reads structure from the input stream.
    ///
    /// Use `validate()` function for checking whether the result is valid.
    pub fn read(src: &mut dyn ByteIO, size: usize) -> ByteIOResult<Self> {
        if !(size == 14 || size == 16 || size >= 18) {
            return Err(ByteIOError::ReadError);
        }
        let format_tag          = src.read_u16le()?;
        let channels            = src.read_u16le()?;
        let sample_rate         = src.read_u32le()?;
        let avg_bytes_per_sec   = src.read_u32le()?;
        let block_align         = usize::from(src.read_u16le()?);
        let bits_per_sample     = if size > 14 { src.read_u16le()? } else { 8 };
        let mut extradata = None;
        if size > 16 {
            let cb_size         = usize::from(src.read_u16le()?);
            if cb_size > 0 && size > 18 {
                if cb_size > size - 18 {
                    return Err(ByteIOError::ReadError);
                }
                let mut edata = vec![0; cb_size];
                                  src.read_buf(&mut edata)?;
                extradata = Some(edata);
            }
        }
        Ok(Self {
            format_tag, channels, sample_rate, avg_bytes_per_sec,
            block_align, bits_per_sample, extradata
        })
    }
    /// Generates `NihAV` audio information.
    pub fn get_audio_info(&self) -> NAAudioInfo {
        let signed = self.bits_per_sample > 8;
        let soniton = NASoniton::new(self.bits_per_sample as u8, if signed { SONITON_FLAG_SIGNED } else { 0 });
        NAAudioInfo::new(self.sample_rate, self.channels as u8, soniton, self.block_align)
    }
    /// Extracts extradata out of the structure and returns it.
    pub fn take_extradata(&mut self) -> Option<Vec<u8>> { self.extradata.take() }
    /// Reports number of bytes required to store this structure.
    pub fn get_size(&self) -> usize {
        18 + self.extradata.as_ref().map_or_else(|| 0, |edata| edata.len())
    }
    /// Writes structure to the provided output.
    pub fn write(&self, dst: &mut dyn ByteIO) -> ByteIOResult<()> {
        dst.write_u16le(self.format_tag)?;
        dst.write_u16le(self.channels)?;
        dst.write_u32le(self.sample_rate)?;
        dst.write_u32le(self.avg_bytes_per_sec)?;
        dst.write_u16le(self.block_align as u16)?;
        dst.write_u16le(self.bits_per_sample)?;
        if let Some(ref edata) = self.extradata {
            dst.write_u16le(edata.len() as u16)?;
            dst.write_buf(edata.as_slice())?;
        } else {
            dst.write_u16le(0)?;
        }
        Ok(())
    }
}
