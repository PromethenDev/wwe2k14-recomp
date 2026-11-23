//! Audio and image sample format definitions.
//!
//! NihAV does not have a fixed list of supported formats but rather accepts format definitions both for audio and video.
//! In result exotic formats like YUV410+alpha plane that is used by Indeo 4 are supported without any additional case handing.
//! Some common format definitions are provided as constants for convenience.
use std::str::FromStr;
use std::string::*;
use std::fmt;

/// Generic format parsing error.
#[derive(Clone,Copy,Debug,PartialEq)]
pub struct FormatParseError {}

/// Audio format definition.
///
/// The structure describes how audio samples are stored and what characteristics they have.
#[derive(Debug,Copy,Clone,PartialEq)]
pub struct NASoniton {
    /// Bits per sample.
    pub bits:       u8,
    /// Audio format is big-endian.
    pub be:         bool,
    /// Audio samples are packed (e.g. 20-bit audio samples).
    pub packed:     bool,
    /// Audio data is stored in planar format instead of interleaving samples for different channels.
    pub planar:     bool,
    /// Audio data is in floating point format.
    pub float:      bool,
    /// Audio data is signed (usually only 8-bit audio is unsigned).
    pub signed:     bool,
}

/// Flag for specifying that audio format is big-endian in `NASoniton::`[`new`]`()`. Related to [`be`] field of `NASoniton`.
///
/// [`new`]: ./struct.NASoniton.html#method.new
/// [`be`]: ./struct.NASoniton.html#structfield.be
pub const SONITON_FLAG_BE     :u32 = 0x01;
/// Flag for specifying that audio format has packed samples in `NASoniton::`[`new`]`()`. Related to [`packed`] field of `NASoniton`.
///
/// [`new`]: ./struct.NASoniton.html#method.new
/// [`packed`]: ./struct.NASoniton.html#structfield.packed
pub const SONITON_FLAG_PACKED :u32 = 0x02;
/// Flag for specifying that audio data is stored as planar in `NASoniton::`[`new`]`()`. Related to [`planar`] field of `NASoniton`.
///
/// [`new`]: ./struct.NASoniton.html#method.new
/// [`planar`]: ./struct.NASoniton.html#structfield.planar
pub const SONITON_FLAG_PLANAR :u32 = 0x04;
/// Flag for specifying that audio samples are in floating point format in `NASoniton::`[`new`]`()`. Related to [`float`] field of `NASoniton`.
///
/// [`new`]: ./struct.NASoniton.html#method.new
/// [`float`]: ./struct.NASoniton.html#structfield.float
pub const SONITON_FLAG_FLOAT  :u32 = 0x08;
/// Flag for specifying that audio format is signed in `NASoniton::`[`new`]`()`. Related to [`signed`] field of `NASoniton`.
///
/// [`new`]: ./struct.NASoniton.html#method.new
/// [`signed`]: ./struct.NASoniton.html#structfield.signed
pub const SONITON_FLAG_SIGNED :u32 = 0x10;

/// Predefined format for interleaved 8-bit unsigned audio.
pub const SND_U8_FORMAT: NASoniton = NASoniton { bits: 8, be: false, packed: false, planar: false, float: false, signed: false };
/// Predefined format for interleaved 16-bit signed audio.
pub const SND_S16_FORMAT: NASoniton = NASoniton { bits: 16, be: false, packed: false, planar: false, float: false, signed: true };
/// Predefined format for planar 16-bit signed audio.
pub const SND_S16P_FORMAT: NASoniton = NASoniton { bits: 16, be: false, packed: false, planar: true, float: false, signed: true };
/// Predefined format for planar 32-bit signed audio.
pub const SND_S32P_FORMAT: NASoniton = NASoniton { bits: 32, be: false, packed: false, planar: true, float: false, signed: true };
/// Predefined format for planar 32-bit floating point audio.
pub const SND_F32P_FORMAT: NASoniton = NASoniton { bits: 32, be: false, packed: false, planar: true, float: true, signed: true };

impl NASoniton {
    /// Constructs a new audio format definition using flags like [`SONITON_FLAG_BE`].
    ///
    /// [`SONITON_FLAG_BE`]: ./constant.SONITON_FLAG_BE.html
    pub fn new(bits: u8, flags: u32) -> Self {
        let is_be = (flags & SONITON_FLAG_BE) != 0;
        let is_pk = (flags & SONITON_FLAG_PACKED) != 0;
        let is_pl = (flags & SONITON_FLAG_PLANAR) != 0;
        let is_fl = (flags & SONITON_FLAG_FLOAT) != 0;
        let is_sg = (flags & SONITON_FLAG_SIGNED) != 0;
        NASoniton { bits, be: is_be, packed: is_pk, planar: is_pl, float: is_fl, signed: is_sg }
    }

    /// Returns the number of bits per sample.
    pub fn get_bits(self)   -> u8   { self.bits }
    /// Reports whether the format is big-endian.
    pub fn is_be(self)      -> bool { self.be }
    /// Reports whether the format has packed samples.
    pub fn is_packed(self)  -> bool { self.packed }
    /// Reports whether audio data is planar instead of interleaved.
    pub fn is_planar(self)  -> bool { self.planar }
    /// Reports whether audio samples are in floating point format.
    pub fn is_float(self)   -> bool { self.float }
    /// Reports whether audio samples are signed.
    pub fn is_signed(self)  -> bool { self.signed }

    /// Returns the amount of bytes needed to store the audio of requested length (in samples).
    pub fn get_audio_size(self, length: u64) -> usize {
        if self.packed {
            ((length * u64::from(self.bits) + 7) >> 3) as usize
        } else {
            (length * u64::from((self.bits + 7) >> 3)) as usize
        }
    }

    /// Returns soniton description as a short string.
    pub fn to_short_string(self) -> String {
        let ltype = if self.float { 'f' } else if self.signed { 's' } else { 'u' };
        let endianness = if self.bits == 8 { "" } else if self.be { "be" } else { "le" };
        let planar = if self.planar { "p" } else { "" };
        let packed = if self.packed { "x" } else { "" };
        format!("{}{}{}{}{}", ltype, self.bits, endianness, planar, packed)
    }
}

impl fmt::Display for NASoniton {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        let fmt = if self.float { "float" } else if self.signed { "int" } else { "uint" };
        let end = if self.be { "BE" } else { "LE" };
        write!(f, "({} bps, {} planar: {} packed: {} {})", self.bits, end, self.planar, self.packed, fmt)
    }
}

impl FromStr for NASoniton {
    type Err = FormatParseError;

    fn from_str(s: &str) -> Result<Self, Self::Err> {
        match s {
            "u8" => Ok(NASoniton { bits: 8, be: true, packed: false, planar: false, float: false, signed: false }),
            "s16be" => Ok(NASoniton { bits: 16, be:  true, packed: false, planar: false, float: false, signed: true }),
            "s16le" => Ok(NASoniton { bits: 16, be: false, packed: false, planar: false, float: false, signed: true }),
            "s24be" => Ok(NASoniton { bits: 24, be:  true, packed: false, planar: false, float: false, signed: true }),
            "s24le" => Ok(NASoniton { bits: 24, be: false, packed: false, planar: false, float: false, signed: true }),
            "s32be" => Ok(NASoniton { bits: 32, be:  true, packed: false, planar: false, float: false, signed: true }),
            "s32le" => Ok(NASoniton { bits: 32, be: false, packed: false, planar: false, float: false, signed: true }),
            "f32be" => Ok(NASoniton { bits: 32, be:  true, packed: false, planar: false, float: true, signed: true }),
            "f32le" => Ok(NASoniton { bits: 32, be: false, packed: false, planar: false, float: true, signed: true }),
            _ => Err(FormatParseError{}),
        }
    }
}

/// Known channel types.
#[derive(Debug,Clone,Copy,PartialEq)]
pub enum NAChannelType {
    C, L, R, Cs, Ls, Rs, Lss, Rss, LFE, Lc, Rc, Lh, Rh, Ch, LFE2, Lw, Rw, Ov, Lhs, Rhs, Chs, Ll, Rl, Cl, Lt, Rt, Lo, Ro
}

impl NAChannelType {
    /// Reports whether this is some centre channel.
    pub fn is_centre(self) -> bool {
        matches!(self,
            NAChannelType::C   | NAChannelType::Ch |
            NAChannelType::Cl  | NAChannelType::Ov |
            NAChannelType::LFE | NAChannelType::LFE2 |
            NAChannelType::Cs  | NAChannelType::Chs)
    }
    /// Reports whether this is some left channel.
    pub fn is_left(self) -> bool {
        matches!(self,
            NAChannelType::L   | NAChannelType::Ls |
            NAChannelType::Lss | NAChannelType::Lc |
            NAChannelType::Lh  | NAChannelType::Lw |
            NAChannelType::Lhs | NAChannelType::Ll |
            NAChannelType::Lt  | NAChannelType::Lo)
    }
    /// Reports whether this is some right channel.
    pub fn is_right(self) -> bool {
        matches!(self,
            NAChannelType::R   | NAChannelType::Rs |
            NAChannelType::Rss | NAChannelType::Rc |
            NAChannelType::Rh  | NAChannelType::Rw |
            NAChannelType::Rhs | NAChannelType::Rl |
            NAChannelType::Rt  | NAChannelType::Ro)
    }
    /// Returns binaural channel pair corresponding to the provided centre channel (or `None` otherwise).
    pub fn get_pair(self) -> Option<(Self, Self)> {
        match self {
            NAChannelType::C => Some((NAChannelType::L, NAChannelType::R)),
            NAChannelType::Ch => Some((NAChannelType::Lh, NAChannelType::Rh)),
            NAChannelType::Cl => Some((NAChannelType::Ll, NAChannelType::Rl)),
            NAChannelType::Cs => Some((NAChannelType::Ls, NAChannelType::Rs)),
            NAChannelType::Chs => Some((NAChannelType::Lhs, NAChannelType::Rhs)),
            _ => None
        }
    }
}

impl FromStr for NAChannelType {
    type Err = FormatParseError;

    fn from_str(s: &str) -> Result<Self, Self::Err> {
        match s {
            "C"     => Ok(NAChannelType::C),
            "L"     => Ok(NAChannelType::L),
            "R"     => Ok(NAChannelType::R),
            "Cs"    => Ok(NAChannelType::Cs),
            "Ls"    => Ok(NAChannelType::Ls),
            "Rs"    => Ok(NAChannelType::Rs),
            "Lss"   => Ok(NAChannelType::Lss),
            "Rss"   => Ok(NAChannelType::Rss),
            "LFE"   => Ok(NAChannelType::LFE),
            "Lc"    => Ok(NAChannelType::Lc),
            "Rc"    => Ok(NAChannelType::Rc),
            "Lh"    => Ok(NAChannelType::Lh),
            "Rh"    => Ok(NAChannelType::Rh),
            "Ch"    => Ok(NAChannelType::Ch),
            "LFE2"  => Ok(NAChannelType::LFE2),
            "Lw"    => Ok(NAChannelType::Lw),
            "Rw"    => Ok(NAChannelType::Rw),
            "Ov"    => Ok(NAChannelType::Ov),
            "Lhs"   => Ok(NAChannelType::Lhs),
            "Rhs"   => Ok(NAChannelType::Rhs),
            "Chs"   => Ok(NAChannelType::Chs),
            "Ll"    => Ok(NAChannelType::Ll),
            "Rl"    => Ok(NAChannelType::Rl),
            "Cl"    => Ok(NAChannelType::Cl),
            "Lt"    => Ok(NAChannelType::Lt),
            "Rt"    => Ok(NAChannelType::Rt),
            "Lo"    => Ok(NAChannelType::Lo),
            "Ro"    => Ok(NAChannelType::Ro),
            _   => Err(FormatParseError{}),
        }
    }
}

impl fmt::Display for NAChannelType {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        let name = match *self {
            NAChannelType::C    => "C".to_string(),
            NAChannelType::L    => "L".to_string(),
            NAChannelType::R    => "R".to_string(),
            NAChannelType::Cs   => "Cs".to_string(),
            NAChannelType::Ls   => "Ls".to_string(),
            NAChannelType::Rs   => "Rs".to_string(),
            NAChannelType::Lss  => "Lss".to_string(),
            NAChannelType::Rss  => "Rss".to_string(),
            NAChannelType::LFE  => "LFE".to_string(),
            NAChannelType::Lc   => "Lc".to_string(),
            NAChannelType::Rc   => "Rc".to_string(),
            NAChannelType::Lh   => "Lh".to_string(),
            NAChannelType::Rh   => "Rh".to_string(),
            NAChannelType::Ch   => "Ch".to_string(),
            NAChannelType::LFE2 => "LFE2".to_string(),
            NAChannelType::Lw   => "Lw".to_string(),
            NAChannelType::Rw   => "Rw".to_string(),
            NAChannelType::Ov   => "Ov".to_string(),
            NAChannelType::Lhs  => "Lhs".to_string(),
            NAChannelType::Rhs  => "Rhs".to_string(),
            NAChannelType::Chs  => "Chs".to_string(),
            NAChannelType::Ll   => "Ll".to_string(),
            NAChannelType::Rl   => "Rl".to_string(),
            NAChannelType::Cl   => "Cl".to_string(),
            NAChannelType::Lt   => "Lt".to_string(),
            NAChannelType::Rt   => "Rt".to_string(),
            NAChannelType::Lo   => "Lo".to_string(),
            NAChannelType::Ro   => "Ro".to_string(),
        };
        write!(f, "{}", name)
    }
}

/// Channel map.
///
/// This is essentially an ordered sequence of channels.
#[derive(Clone,Default)]
pub struct NAChannelMap {
    ids: Vec<NAChannelType>,
}

const MS_CHANNEL_MAP: [NAChannelType; 11] = [
    NAChannelType::L,
    NAChannelType::R,
    NAChannelType::C,
    NAChannelType::LFE,
    NAChannelType::Ls,
    NAChannelType::Rs,
    NAChannelType::Lss,
    NAChannelType::Rss,
    NAChannelType::Cs,
    NAChannelType::Lc,
    NAChannelType::Rc,
];

impl NAChannelMap {
    /// Constructs a new `NAChannelMap` instance.
    pub fn new() -> Self { NAChannelMap { ids: Vec::new() } }
    /// Adds a new channel to the map.
    pub fn add_channel(&mut self, ch: NAChannelType) {
        self.ids.push(ch);
    }
    /// Adds several channels to the map at once.
    pub fn add_channels(&mut self, chs: &[NAChannelType]) {
        for e in chs.iter() {
            self.ids.push(*e);
        }
    }
    /// Returns the total number of channels.
    pub fn num_channels(&self) -> usize {
        self.ids.len()
    }
    /// Reports channel type for a requested index.
    pub fn get_channel(&self, idx: usize) -> NAChannelType {
        self.ids[idx]
    }
    /// Tries to find position of the channel with requested type.
    pub fn find_channel_id(&self, t: NAChannelType) -> Option<usize> {
        (0..self.ids.len()).find(|&i| self.ids[i] as i32 == t as i32)
    }
    /// Creates a new `NAChannelMap` using the channel mapping flags from WAVE format.
    pub fn from_ms_mapping(chmap: u32) -> Self {
        let mut cm = NAChannelMap::new();
        for (i, ch) in MS_CHANNEL_MAP.iter().enumerate() {
            if ((chmap >> i) & 1) != 0 {
                cm.add_channel(*ch);
            }
        }
        cm
    }
    /// Returns an iterator over channels.
    pub fn iter(&self) -> std::slice::Iter<'_, NAChannelType> { self.ids.iter() }
}

impl fmt::Display for NAChannelMap {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        let mut map = String::new();
        for el in self.ids.iter() {
            if !map.is_empty() { map.push(','); }
            map.push_str(&el.to_string());
        }
        write!(f, "{}", map)
    }
}

impl FromStr for NAChannelMap {
    type Err = FormatParseError;

    fn from_str(s: &str) -> Result<Self, Self::Err> {
        let mut chm = NAChannelMap::new();
        for tok in s.split(',') {
            chm.add_channel(NAChannelType::from_str(tok)?);
        }
        Ok(chm)
    }
}

/// A list of RGB colour model variants.
#[derive(Debug,Clone,Copy,PartialEq)]
pub enum RGBSubmodel {
    RGB,
    SRGB,
}

impl fmt::Display for RGBSubmodel {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        let name = match *self {
            RGBSubmodel::RGB  => "RGB".to_string(),
            RGBSubmodel::SRGB => "sRGB".to_string(),
        };
        write!(f, "{}", name)
    }
}

/// A list of YUV colour model variants.
#[derive(Debug,Clone,Copy,PartialEq)]
pub enum YUVSubmodel {
    YCbCr,
    /// NTSC variant.
    YIQ,
    /// The YUV variant used by JPEG.
    YUVJ,
}

impl fmt::Display for YUVSubmodel {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        let name = match *self {
            YUVSubmodel::YCbCr => "YCbCr".to_string(),
            YUVSubmodel::YIQ   => "YIQ".to_string(),
            YUVSubmodel::YUVJ  => "YUVJ".to_string(),
        };
        write!(f, "{}", name)
    }
}

/// A list of known colour models.
#[derive(Debug, Clone,Copy,PartialEq)]
pub enum ColourModel {
    RGB(RGBSubmodel),
    YUV(YUVSubmodel),
    CMYK,
    HSV,
    LAB,
    XYZ,
}

impl ColourModel {
    /// Returns the number of colour model components.
    ///
    /// The actual image may have more components e.g. alpha component.
    pub fn get_default_components(self) -> usize {
        match self {
            ColourModel::CMYK => 4,
            _                 => 3,
        }
    }
    /// Reports whether the current colour model is RGB.
    pub fn is_rgb(self) -> bool {
        matches!(self, ColourModel::RGB(_))
    }
    /// Reports whether the current colour model is YUV.
    pub fn is_yuv(self) -> bool {
        matches!(self, ColourModel::YUV(_))
    }
    /// Returns short name for the current colour mode.
    pub fn get_short_name(self) -> &'static str {
        match self {
            ColourModel::RGB(_)   => "rgb",
            ColourModel::YUV(_)   => "yuv",
            ColourModel::CMYK     => "cmyk",
            ColourModel::HSV      => "hsv",
            ColourModel::LAB      => "lab",
            ColourModel::XYZ      => "xyz",
        }
    }
}

impl fmt::Display for ColourModel {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        let name = match *self {
            ColourModel::RGB(fmt) => format!("RGB({})", fmt),
            ColourModel::YUV(fmt) => format!("YUV({})", fmt),
            ColourModel::CMYK     => "CMYK".to_string(),
            ColourModel::HSV      => "HSV".to_string(),
            ColourModel::LAB      => "LAB".to_string(),
            ColourModel::XYZ      => "XYZ".to_string(),
        };
        write!(f, "{}", name)
    }
}

/// Single colourspace component definition.
///
/// This structure defines how components of a colourspace are subsampled and where and how they are stored.
#[derive(Debug,Clone,Copy,PartialEq)]
pub struct NAPixelChromaton {
    /// Horizontal subsampling in power of two (e.g. `0` = no subsampling, `1` = only every second value is stored).
    pub h_ss:           u8,
    /// Vertial subsampling in power of two (e.g. `0` = no subsampling, `1` = only every second value is stored).
    pub v_ss:           u8,
    /// A flag to signal that component is packed.
    pub packed:         bool,
    /// Bit depth of current component.
    pub depth:          u8,
    /// Shift for packed components.
    pub shift:          u8,
    /// Component offset for byte-packed components.
    pub comp_offs:      u8,
    /// The distance to the next packed element in bytes.
    pub next_elem:      u8,
}

/// Flag for specifying that image data is stored big-endian in `NAPixelFormaton::`[`new`]`()`. Related to its [`be`] field.
///
/// [`new`]: ./struct.NAPixelFormaton.html#method.new
/// [`be`]: ./struct.NAPixelFormaton.html#structfield.new
pub const FORMATON_FLAG_BE      :u32 = 0x01;
/// Flag for specifying that image data has alpha plane in `NAPixelFormaton::`[`new`]`()`. Related to its [`alpha`] field.
///
/// [`new`]: ./struct.NAPixelFormaton.html#method.new
/// [`alpha`]: ./struct.NAPixelFormaton.html#structfield.alpha
pub const FORMATON_FLAG_ALPHA   :u32 = 0x02;
/// Flag for specifying that image data is stored in paletted form for `NAPixelFormaton::`[`new`]`()`. Related to its [`palette`] field.
///
/// [`new`]: ./struct.NAPixelFormaton.html#method.new
/// [`palette`]: ./struct.NAPixelFormaton.html#structfield.palette
pub const FORMATON_FLAG_PALETTE :u32 = 0x04;

/// The current limit on number of components in image colourspace model (including alpha component).
pub const MAX_CHROMATONS: usize = 5;

/// Image colourspace representation.
///
/// This structure includes both definitions for each component and some common definitions.
/// For example the format can be paletted and then components describe the palette storage format while actual data is 8-bit palette indices.
#[derive(Debug,Clone,Copy,PartialEq)]
pub struct NAPixelFormaton {
    /// Image colour model.
    pub model:      ColourModel,
    /// Actual number of components present.
    pub components: u8,
    /// Format definition for each component.
    pub comp_info:  [Option<NAPixelChromaton>; MAX_CHROMATONS],
    /// Single pixel size for packed formats.
    pub elem_size:  u8,
    /// A flag signalling that data is stored as big-endian.
    pub be:         bool,
    /// A flag signalling that image has alpha component.
    pub alpha:      bool,
    /// A flag signalling that data is paletted.
    ///
    /// This means that image data is stored as 8-bit indices (in the first image component) for the palette stored as second component of the image and actual palette format is described in this structure.
    pub palette:    bool,
}

macro_rules! chromaton {
    ($hs: expr, $vs: expr, $pck: expr, $d: expr, $sh: expr, $co: expr, $ne: expr) => ({
        Some(NAPixelChromaton{ h_ss: $hs, v_ss: $vs, packed: $pck, depth: $d, shift: $sh, comp_offs: $co, next_elem: $ne })
    });
    (yuv8; $hs: expr, $vs: expr, $co: expr) => ({
        Some(NAPixelChromaton{ h_ss: $hs, v_ss: $vs, packed: false, depth: 8, shift: 0, comp_offs: $co, next_elem: 1 })
    });
    (packrgb; $d: expr, $s: expr, $co: expr, $ne: expr) => ({
        Some(NAPixelChromaton{ h_ss: 0, v_ss: 0, packed: true, depth: $d, shift: $s, comp_offs: $co, next_elem: $ne })
    });
    (pal8; $co: expr) => ({
        Some(NAPixelChromaton{ h_ss: 0, v_ss: 0, packed: true, depth: 8, shift: 0, comp_offs: $co, next_elem: 3 })
    });
}

/// Predefined format for planar 8-bit YUV with 4:2:0 subsampling.
pub const YUV420_FORMAT: NAPixelFormaton = NAPixelFormaton { model: ColourModel::YUV(YUVSubmodel::YUVJ), components: 3,
                                        comp_info: [
                                            chromaton!(0, 0, false, 8, 0, 0, 1),
                                            chromaton!(yuv8; 1, 1, 1),
                                            chromaton!(yuv8; 1, 1, 2),
                                            None, None],
                                        elem_size: 0, be: false, alpha: false, palette: false };

/// Predefined format for planar 8-bit YUV with 4:1:0 subsampling.
pub const YUV410_FORMAT: NAPixelFormaton = NAPixelFormaton { model: ColourModel::YUV(YUVSubmodel::YUVJ), components: 3,
                                        comp_info: [
                                            chromaton!(0, 0, false, 8, 0, 0, 1),
                                            chromaton!(yuv8; 2, 2, 1),
                                            chromaton!(yuv8; 2, 2, 2),
                                            None, None],
                                        elem_size: 0, be: false, alpha: false, palette: false };
/// Predefined format for planar 8-bit YUV with 4:1:0 subsampling and alpha component.
pub const YUVA410_FORMAT: NAPixelFormaton = NAPixelFormaton { model: ColourModel::YUV(YUVSubmodel::YUVJ), components: 4,
                                        comp_info: [
                                            chromaton!(0, 0, false, 8, 0, 0, 1),
                                            chromaton!(yuv8; 2, 2, 1),
                                            chromaton!(yuv8; 2, 2, 2),
                                            chromaton!(0, 0, false, 8, 0, 3, 1),
                                            None],
                                        elem_size: 0, be: false, alpha: true, palette: false };

/// Predefined format with RGB24 palette.
pub const PAL8_FORMAT: NAPixelFormaton = NAPixelFormaton { model: ColourModel::RGB(RGBSubmodel::RGB), components: 3,
                                        comp_info: [
                                            chromaton!(pal8; 0),
                                            chromaton!(pal8; 1),
                                            chromaton!(pal8; 2),
                                            None, None],
                                        elem_size: 3, be: false, alpha: false, palette: true };

/// Pre-defined format for 8-bit greyscale.
pub const GREY_FORMAT: NAPixelFormaton = NAPixelFormaton { model: ColourModel::YUV(YUVSubmodel::YUVJ), components: 1,
                                        comp_info: [
                                            chromaton!(0, 0, false, 8, 0, 0, 1),
                                            None, None, None, None],
                                        elem_size: 1, be: true, alpha: false, palette: false };

/// Predefined format for RGB555 packed video.
pub const RGB555_FORMAT: NAPixelFormaton = NAPixelFormaton::make_rgb16_fmt(5, 5, 5, true, false);

/// Predefined format for RGB565 packed video.
pub const RGB565_FORMAT: NAPixelFormaton = NAPixelFormaton::make_rgb16_fmt(5, 6, 5, true, false);

/// Predefined format for RGB24.
pub const RGB24_FORMAT: NAPixelFormaton = NAPixelFormaton { model: ColourModel::RGB(RGBSubmodel::RGB), components: 3,
                                        comp_info: [
                                            chromaton!(packrgb; 8, 0, 0, 3),
                                            chromaton!(packrgb; 8, 0, 1, 3),
                                            chromaton!(packrgb; 8, 0, 2, 3),
                                            None, None],
                                        elem_size: 3, be: false, alpha: false, palette: false };

impl NAPixelChromaton {
    /// Constructs a new `NAPixelChromaton` instance.
    pub fn new(h_ss: u8, v_ss: u8, packed: bool, depth: u8, shift: u8, comp_offs: u8, next_elem: u8) -> Self {
        Self { h_ss, v_ss, packed, depth, shift, comp_offs, next_elem }
    }
    /// Returns subsampling for the current component.
    pub fn get_subsampling(self) -> (u8, u8) { (self.h_ss, self.v_ss) }
    /// Reports whether current component is packed.
    pub fn is_packed(self) -> bool { self.packed }
    /// Returns bit depth of current component.
    pub fn get_depth(self) -> u8   { self.depth }
    /// Returns bit shift for packed component.
    pub fn get_shift(self) -> u8   { self.shift }
    /// Returns byte offset for packed component.
    pub fn get_offset(self) -> u8  { self.comp_offs }
    /// Returns byte offset to the next element of current packed component.
    pub fn get_step(self)  -> u8   { self.next_elem }

    /// Calculates the width for current component from general image width.
    pub fn get_width(self, width: usize) -> usize {
        (width  + ((1 << self.h_ss) - 1)) >> self.h_ss
    }
    /// Calculates the height for current component from general image height.
    pub fn get_height(self, height: usize) -> usize {
        (height + ((1 << self.v_ss) - 1)) >> self.v_ss
    }
    /// Calculates the minimal stride for current component from general image width.
    pub fn get_linesize(self, width: usize) -> usize {
        let d = self.depth as usize;
        if self.packed {
            (self.get_width(width) * d + d - 1) >> 3
        } else {
            self.get_width(width)
        }
    }
    /// Calculates the required image size in pixels for current component from general image width.
    pub fn get_data_size(self, width: usize, height: usize) -> usize {
        let nh = (height + ((1 << self.v_ss) - 1)) >> self.v_ss;
        self.get_linesize(width) * nh
    }
}

impl fmt::Display for NAPixelChromaton {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        let pfmt = if self.packed {
            let mask = ((1 << self.depth) - 1) << self.shift;
            format!("packed(+{},{:X}, step {})", self.comp_offs, mask, self.next_elem)
        } else {
            format!("planar({},{})", self.comp_offs, self.next_elem)
        };
        write!(f, "({}x{}, {})", self.h_ss, self.v_ss, pfmt)
    }
}

impl NAPixelFormaton {
    /// Constructs a new instance of `NAPixelFormaton`.
    pub fn new(model: ColourModel,
               comp1: Option<NAPixelChromaton>,
               comp2: Option<NAPixelChromaton>,
               comp3: Option<NAPixelChromaton>,
               comp4: Option<NAPixelChromaton>,
               comp5: Option<NAPixelChromaton>,
               flags: u32, elem_size: u8) -> Self {
        let mut chromatons: [Option<NAPixelChromaton>; MAX_CHROMATONS] = [None; MAX_CHROMATONS];
        let mut ncomp = 0;
        let be      = (flags & FORMATON_FLAG_BE)      != 0;
        let alpha   = (flags & FORMATON_FLAG_ALPHA)   != 0;
        let palette = (flags & FORMATON_FLAG_PALETTE) != 0;
        if let Some(c) = comp1 { chromatons[0] = Some(c); ncomp += 1; }
        if let Some(c) = comp2 { chromatons[1] = Some(c); ncomp += 1; }
        if let Some(c) = comp3 { chromatons[2] = Some(c); ncomp += 1; }
        if let Some(c) = comp4 { chromatons[3] = Some(c); ncomp += 1; }
        if let Some(c) = comp5 { chromatons[4] = Some(c); ncomp += 1; }
        NAPixelFormaton { model,
                          components: ncomp,
                          comp_info: chromatons,
                          elem_size,
                          be, alpha, palette }
    }

    /// Returns current colour model.
    pub fn get_model(&self) -> ColourModel { self.model }
    /// Returns the number of components.
    pub fn get_num_comp(&self) -> usize { self.components as usize }
    /// Returns selected component information.
    pub fn get_chromaton(&self, idx: usize) -> Option<NAPixelChromaton> {
        if idx < self.comp_info.len() { return self.comp_info[idx]; }
        None
    }
    /// Reports whether the packing format is big-endian.
    pub fn is_be(self) -> bool { self.be }
    /// Reports whether colourspace has alpha component.
    pub fn has_alpha(self) -> bool { self.alpha }
    /// Reports whether this is paletted format.
    pub fn is_paletted(self) -> bool { self.palette }
    /// Returns single packed pixel size.
    pub fn get_elem_size(self) -> u8 { self.elem_size }
    /// Reports whether the format is not packed.
    pub fn is_unpacked(&self) -> bool {
        if self.palette { return false; }
        for chromaton in self.comp_info.iter().flatten() {
            if chromaton.is_packed() { return false; }
        }
        true
    }
    /// Returns the maximum component bit depth.
    pub fn get_max_depth(&self) -> u8 {
        let mut mdepth = 0;
        for chromaton in self.comp_info.iter().flatten() {
            mdepth = mdepth.max(chromaton.depth);
        }
        mdepth
    }
    /// Returns the total amount of bits needed for components.
    pub fn get_total_depth(&self) -> u8 {
        let mut depth = 0;
        for chromaton in self.comp_info.iter().flatten() {
            depth += chromaton.depth;
        }
        depth
    }
    /// Returns the maximum component subsampling.
    pub fn get_max_subsampling(&self) -> u8 {
        let mut ssamp = 0;
        for chromaton in self.comp_info.iter().flatten() {
            let (ss_v, ss_h) = chromaton.get_subsampling();
            ssamp = ssamp.max(ss_v).max(ss_h);
        }
        ssamp
    }
    #[allow(clippy::cognitive_complexity)]
    /// Returns a short string description of the format if possible.
    pub fn to_short_string(&self) -> Option<String> {
        match self.model {
            ColourModel::RGB(_) => {
                if self.is_paletted() {
                    if *self == PAL8_FORMAT {
                        return Some("pal8".to_string());
                    } else {
                        return None;
                    }
                }
                let mut name = [b'z'; 4];
                let planar = self.is_unpacked();

                let mut use_comp_offs = false;
                for comp in self.comp_info.iter().flatten() {
                    if comp.comp_offs != 0 {
                        use_comp_offs = true;
                        break;
                    }
                }

                const RGB_CNAMES: &[u8] = b"rgba";
                let mut co: [(u8, Option<NAPixelChromaton>); 4] = std::array::from_fn(|i| (RGB_CNAMES[i], self.comp_info[i]));
                if use_comp_offs {
                    co.sort_by_key(|el| if let Some(ci) = el.1 { ci.comp_offs } else { 99 });
                } else {
                    let depth = self.get_total_depth();
                    co.sort_by_key(|el| if let Some(ci) = el.1 { depth - ci.shift } else { 255 });
                }

                for (dname, (c, ci)) in name.iter_mut().zip(co.iter()) {
                    if ci.is_none() {
                        break;
                    }
                    *dname = *c;
                }

                let mut name = String::from_utf8(name[..self.components as usize].to_vec()).unwrap();
                let depth = self.get_total_depth();
                if depth == 15 || depth == 16 {
                    for c in self.comp_info.iter() {
                        if let Some(comp) = c {
                            name.push((b'0' + comp.depth) as char);
                        } else {
                            break;
                        }
                    }
                    name += if self.be { "be" } else { "le" };
                    return Some(name);
                }
                if depth == 24 || depth != 8 * self.components {
                    name += depth.to_string().as_str();
                }
                if planar {
                    name.push('p');
                }
                if self.get_max_depth() > 8 {
                    name += if self.be { "be" } else { "le" };
                }
                Some(name)
            },
            ColourModel::YUV(_) => {
                let max_depth = self.get_max_depth();
                if self.get_total_depth() != max_depth * self.components {
                    return None;
                }
                if self.components < 3 {
                    if self.components == 1 && max_depth == 8 {
                        return Some("y8".to_string());
                    }
                    if self.components == 2 && self.alpha && max_depth == 8 {
                        return Some("y8a".to_string());
                    }
                    return None;
                }
                let cu = self.comp_info[1].unwrap();
                let cv = self.comp_info[2].unwrap();
                if cu.h_ss != cv.h_ss || cu.v_ss != cv.v_ss || cu.h_ss > 2 || cu.v_ss > 2 {
                    return None;
                }
                let mut name = "yuv".to_string();
                if self.alpha {
                    name.push('a');
                }
                name.push('4');
                match (cu.h_ss, cu.v_ss) {
                    (0, 0) => name += "44",
                    (0, 1) => name += "40",
                    (1, 0) => name += "22",
                    (1, 1) => name += "20",
                    (2, 0) => name += "11",
                    (2, 2) => name += "10",
                    _ => return None,
                }
                if self.is_unpacked() {
                    name.push('p');
                }
                if max_depth != 8 {
                    name += max_depth.to_string().as_str();
                }
                Some(name)
            },
            _ => None,
        }
    }
    /// Constructs a 15/16-bit RGB `NAPixelFormaton` from a compact description.
    pub const fn make_rgb16_fmt(rbits: u8, gbits: u8, bbits: u8, rtop: bool, be: bool) -> Self {
        let (rshift, gshift, bshift) = if rtop { (gbits + bbits, bbits, 0) } else { (0, rbits, rbits + gbits) };
        let chromatons = [
            Some(NAPixelChromaton { h_ss: 0, v_ss: 0, packed: true, depth: rbits, shift: rshift, comp_offs: 0, next_elem: 2}),
            Some(NAPixelChromaton { h_ss: 0, v_ss: 0, packed: true, depth: gbits, shift: gshift, comp_offs: 0, next_elem: 2}),
            Some(NAPixelChromaton { h_ss: 0, v_ss: 0, packed: true, depth: bbits, shift: bshift, comp_offs: 0, next_elem: 2}),
                None, None
        ];
        NAPixelFormaton { model: ColourModel::RGB(RGBSubmodel::RGB),
                          components: 3,
                          comp_info: chromatons,
                          elem_size: 2,
                          be, alpha: false, palette: false }
    }
}

impl fmt::Display for NAPixelFormaton {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        let end = if self.be { "BE" } else { "LE" };
        let palstr = if self.palette { "palette " } else { "" };
        let astr = if self.alpha { "alpha " } else { "" };
        let mut string = format!("Formaton for {} ({}{}elem {} size {}): ", self.model, palstr, astr,end, self.elem_size);
        for i in 0..self.comp_info.len() {
            if let Some(chr) = self.comp_info[i] {
                string = format!("{} {}", string, chr);
            }
        }
        write!(f, "[{}]", string)
    }
}

fn parse_rgb_format(s: &str) -> Result<NAPixelFormaton, FormatParseError> {
    let mut order = [0; 4];
    let mut is_be = s.ends_with("be");
    let mut has_alpha = false;
    let mut pstate = 0;
    let mut bits = 0;
    let mut bits_start = 0;
    for (i, ch) in s.chars().enumerate() {
        match pstate {
            0 => {
                if i > 4 { return Err(FormatParseError {}); }
                match ch {
                    'R' | 'r' => { order[0] = i; },
                    'G' | 'g' => { order[1] = i; },
                    'B' | 'b' => { order[2] = i; },
                    'A' | 'a' => { order[3] = i; has_alpha = true; },
                    '0'..='9' => {
                        pstate = 1; bits_start = i;
                        bits = u32::from((ch as u8) - b'0');
                    },
                    _ => return Err(FormatParseError {}),
                };
            },
            1 => {
                if i > 4 + bits_start { return Err(FormatParseError {}); }
                match ch {
                    '0'..='9' => {
                        bits = (bits * 10) + u32::from((ch as u8) - b'0');
                    },
                    'B' | 'b' => { pstate = 2; }
                    'L' | 'l' => { pstate = 2; is_be = false; }
                    _ => return Err(FormatParseError {}),
                }
            },
            2 => {
                if ch != 'e' && ch != 'E' { return Err(FormatParseError {}); }
                pstate = 3;
            },
            _ => return Err(FormatParseError {}),
        };
    }
    let components: u8 = if has_alpha { 4 } else { 3 };
    for el in order.iter() {
        if *el >= (components as usize) {
            return Err(FormatParseError {});
        }
    }
    if order[0] == order[1] || order[0] == order[2] || order[1] == order[2] {
        return Err(FormatParseError {});
    }
    if has_alpha && order[0..3].contains(&order[3]) {
        return Err(FormatParseError {});
    }
    let mut chromatons = [None; 5];
    let elem_size = match bits {
            0 | 24 => {
                for (chro, ord) in chromatons.iter_mut().take(components as usize).zip(order.iter()) {
                    *chro = Some(NAPixelChromaton { h_ss: 0, v_ss: 0, packed: true, depth: 8, shift: 0, comp_offs: *ord as u8, next_elem: components });
                }
                components
            },
            555 => {
                let rshift = (10 - order[0] * 5) as u8;
                let gshift = (10 - order[1] * 5) as u8;
                let bshift = (10 - order[2] * 5) as u8;
                chromatons[0] = Some(NAPixelChromaton { h_ss: 0, v_ss: 0, packed: true, depth: 5, shift: rshift, comp_offs: 0, next_elem: 2 });
                chromatons[1] = Some(NAPixelChromaton { h_ss: 0, v_ss: 0, packed: true, depth: 5, shift: gshift, comp_offs: 0, next_elem: 2 });
                chromatons[2] = Some(NAPixelChromaton { h_ss: 0, v_ss: 0, packed: true, depth: 5, shift: bshift, comp_offs: 0, next_elem: 2 });
                if has_alpha { return Err(FormatParseError {}); }
                2
            },
            565 => {
                let mut offs = [0; 3];
                for (ord, off) in order.iter().zip(offs.iter_mut()) {
                    *off = (10 - *ord * 5) as u8;
                }
                match order[1] {
                    0 => { offs[0] += 1; offs[2] += 1; },
                    1 => { for el in offs.iter_mut() { if *el == 10 { *el += 1; break; } } },
                    _ => {},
                };
                chromatons[0] = Some(NAPixelChromaton { h_ss: 0, v_ss: 0, packed: true, depth: 5, shift: offs[0], comp_offs: 0, next_elem: 2 });
                chromatons[1] = Some(NAPixelChromaton { h_ss: 0, v_ss: 0, packed: true, depth: 6, shift: offs[1], comp_offs: 0, next_elem: 2 });
                chromatons[2] = Some(NAPixelChromaton { h_ss: 0, v_ss: 0, packed: true, depth: 5, shift: offs[2], comp_offs: 0, next_elem: 2 });
                if has_alpha { return Err(FormatParseError {}); }
                2
            },
            5551 => {
                let mut offs = [0; 4];
                let mut cur_off = 16;
                for (comp, &depth) in [ 5, 5, 5, 1 ].iter().enumerate() {
                    for (off, ord) in offs.iter_mut().zip(order.iter()) {
                        if *ord == comp {
                            cur_off -= depth;
                            *off = cur_off;
                            break;
                        }
                    }
                }
                chromatons[0] = Some(NAPixelChromaton { h_ss: 0, v_ss: 0, packed: true, depth: 5, shift: offs[0], comp_offs: 0, next_elem: 2 });
                chromatons[1] = Some(NAPixelChromaton { h_ss: 0, v_ss: 0, packed: true, depth: 5, shift: offs[1], comp_offs: 0, next_elem: 2 });
                chromatons[2] = Some(NAPixelChromaton { h_ss: 0, v_ss: 0, packed: true, depth: 5, shift: offs[2], comp_offs: 0, next_elem: 2 });
                chromatons[3] = Some(NAPixelChromaton { h_ss: 0, v_ss: 0, packed: true, depth: 1, shift: offs[3], comp_offs: 0, next_elem: 2 });
                if !has_alpha { return Err(FormatParseError {}); }
                2
            },
            _ => return Err(FormatParseError {}),
        };
    Ok(NAPixelFormaton { model: ColourModel::RGB(RGBSubmodel::RGB),
                         components,
                         comp_info: chromatons,
                         elem_size,
                         be: is_be, alpha: has_alpha, palette: false })
}

fn parse_yuv_format(s: &str) -> Result<NAPixelFormaton, FormatParseError> {
    match s {
        "y8" | "y400" | "grey" | "gray" => {
            return Ok(NAPixelFormaton {
                    model: ColourModel::YUV(YUVSubmodel::YUVJ), components: 1,
                    comp_info: [
                        Some(NAPixelChromaton{ h_ss: 0, v_ss: 0, packed: false, depth: 8, shift: 0, comp_offs: 0, next_elem: 1 }),
                        None, None, None, None],
                    elem_size: 1, be: true, alpha: false, palette: false });
        },
        "y8a" | "y400a" | "greya" | "graya" => {
            return Ok(NAPixelFormaton {
                    model: ColourModel::YUV(YUVSubmodel::YUVJ), components: 2,
                    comp_info: [
                        Some(NAPixelChromaton{ h_ss: 0, v_ss: 0, packed: false, depth: 8, shift: 0, comp_offs: 0, next_elem: 2 }),
                        Some(NAPixelChromaton{ h_ss: 0, v_ss: 0, packed: false, depth: 8, shift: 0, comp_offs: 1, next_elem: 2 }),
                        None, None, None],
                    elem_size: 1, be: true, alpha: true, palette: false });
        },
        "uyvy" | "y422" => {
            return Ok(NAPixelFormaton {
                    model: ColourModel::YUV(YUVSubmodel::YUVJ), components: 3,
                    comp_info: [
                        Some(NAPixelChromaton{ h_ss: 0, v_ss: 0, packed: true, depth: 8, shift: 0, comp_offs: 1, next_elem: 2 }),
                        Some(NAPixelChromaton{ h_ss: 1, v_ss: 0, packed: true, depth: 8, shift: 0, comp_offs: 0, next_elem: 4 }),
                        Some(NAPixelChromaton{ h_ss: 1, v_ss: 0, packed: true, depth: 8, shift: 0, comp_offs: 2, next_elem: 4 }),
                        None, None],
                    elem_size: 4, be: false, alpha: false, palette: false });
        },
        "yuy2" | "yuyv" | "v422" => {
            return Ok(NAPixelFormaton {
                    model: ColourModel::YUV(YUVSubmodel::YUVJ), components: 3,
                    comp_info: [
                        Some(NAPixelChromaton{ h_ss: 0, v_ss: 0, packed: true, depth: 8, shift: 0, comp_offs: 0, next_elem: 2 }),
                        Some(NAPixelChromaton{ h_ss: 1, v_ss: 0, packed: true, depth: 8, shift: 0, comp_offs: 1, next_elem: 4 }),
                        Some(NAPixelChromaton{ h_ss: 1, v_ss: 0, packed: true, depth: 8, shift: 0, comp_offs: 3, next_elem: 4 }),
                        None, None],
                    elem_size: 4, be: false, alpha: false, palette: false });
        },
        "yvyu" => {
            return Ok(NAPixelFormaton {
                    model: ColourModel::YUV(YUVSubmodel::YUVJ), components: 3,
                    comp_info: [
                        Some(NAPixelChromaton{ h_ss: 0, v_ss: 0, packed: true, depth: 8, shift: 0, comp_offs: 0, next_elem: 2 }),
                        Some(NAPixelChromaton{ h_ss: 1, v_ss: 0, packed: true, depth: 8, shift: 0, comp_offs: 3, next_elem: 4 }),
                        Some(NAPixelChromaton{ h_ss: 1, v_ss: 0, packed: true, depth: 8, shift: 0, comp_offs: 1, next_elem: 4 }),
                        None, None],
                    elem_size: 4, be: false, alpha: false, palette: false });
        },
        "vyuy" => {
            return Ok(NAPixelFormaton {
                    model: ColourModel::YUV(YUVSubmodel::YUVJ), components: 3,
                    comp_info: [
                        Some(NAPixelChromaton{ h_ss: 0, v_ss: 0, packed: true, depth: 8, shift: 0, comp_offs: 1, next_elem: 2 }),
                        Some(NAPixelChromaton{ h_ss: 1, v_ss: 0, packed: true, depth: 8, shift: 0, comp_offs: 2, next_elem: 4 }),
                        Some(NAPixelChromaton{ h_ss: 1, v_ss: 0, packed: true, depth: 8, shift: 0, comp_offs: 0, next_elem: 4 }),
                        None, None],
                    elem_size: 4, be: false, alpha: false, palette: false });
        },
        _ => {},
    };
    if !s.starts_with("yuv") {
        return Err(FormatParseError {});
    }
    let has_alpha = s.starts_with("yuva");
    let components: u8 = if has_alpha { 4 } else { 3 };
    let mut is_planar = false;
    let mut format = 0;
    let mut parse_end = components as usize;
    for ch in s.chars().skip(components as usize) {
        parse_end += 1;
        if ch.is_ascii_digit() {
            format = format * 10 + u32::from((ch as u8) - b'0');
            if format > 444 { return Err(FormatParseError {}); }
        } else {
            is_planar = ch == 'p';
            break;
        }
    }
    if format == 0 { return Err(FormatParseError {}); }
    let depth = if s.len() == parse_end { 8 } else {
            let mut val = 0;
            for ch in s.chars().skip(parse_end) {
                if ch.is_ascii_digit() {
                    val = val * 10 + ((ch as u8) - b'0');
                    if val > 16 { return Err(FormatParseError {}); }
                } else {
                    break;
                }
            }
            val
        };
    if depth == 0 { return Err(FormatParseError {}); }
    let is_be = s.ends_with("be");

    let mut chromatons = [None; 5];
    let next_elem = if is_planar { (depth + 7) >> 3 } else {
            components * ((depth + 7) >> 3) };
    let subsamp: [[u8; 2]; 4] = match format {
            410 => [[0, 0], [2, 2], [2, 2], [0, 0]],
            411 => [[0, 0], [2, 0], [2, 0], [0, 0]],
            420 => [[0, 0], [1, 1], [1, 1], [0, 0]],
            422 => [[0, 0], [1, 0], [1, 0], [0, 0]],
            440 => [[0, 0], [0, 1], [0, 1], [0, 0]],
            444 => [[0, 0], [0, 0], [0, 0], [0, 0]],
            _ => return Err(FormatParseError {}),
        };
    for (i, (chro, ss)) in chromatons.iter_mut().take(components as usize).zip(subsamp.iter()).enumerate() {
        *chro = Some(NAPixelChromaton{ h_ss: ss[0], v_ss: ss[1], packed: !is_planar, depth, shift: 0, comp_offs: if is_planar { i as u8 } else { next_elem }, next_elem });
    }
    Ok(NAPixelFormaton { model: ColourModel::YUV(YUVSubmodel::YUVJ),
                         components,
                         comp_info: chromatons,
                         elem_size: components,
                         be: is_be, alpha: has_alpha, palette: false })
}

impl FromStr for NAPixelFormaton {
    type Err = FormatParseError;

    #[allow(clippy::single_match)]
    fn from_str(s: &str) -> Result<Self, Self::Err> {
        match s {
            "pal8" => return Ok(PAL8_FORMAT),
            _ => {},
        }
        let ret = parse_rgb_format(s);
        if ret.is_ok() {
            return ret;
        }
        parse_yuv_format(s)
    }
}

#[cfg(test)]
mod test {
    use super::*;

    #[test]
    fn test_audio_fmt_names() {
        println!("{}", SND_S16_FORMAT);
        println!("{}", SND_U8_FORMAT);
        println!("{}", SND_F32P_FORMAT);
        assert_eq!(SND_U8_FORMAT.to_short_string(), "u8");
        assert_eq!(SND_F32P_FORMAT.to_short_string(), "f32lep");
        let s16fmt = SND_S16_FORMAT.to_short_string();
        assert_eq!(NASoniton::from_str(s16fmt.as_str()).unwrap(), SND_S16_FORMAT);
    }
    #[test]
    fn test_rgb_fmt_names() {
        println!("formaton pal - {}", PAL8_FORMAT);
        assert_eq!(PAL8_FORMAT.to_short_string().unwrap(), "pal8");
        println!("formaton rgb565 - {}", RGB565_FORMAT);
        assert_eq!(RGB565_FORMAT.to_short_string().unwrap(), "rgb565le");

        let pfmt = NAPixelFormaton::from_str("rgb24").unwrap();
        assert!(pfmt == RGB24_FORMAT);
        let pfmt = "gbra";
        assert_eq!(pfmt, NAPixelFormaton::from_str("gbra").unwrap().to_short_string().unwrap());

        for &be in [true, false].iter() {
            for &rgb in [true, false].iter() {
                let fmt = NAPixelFormaton::make_rgb16_fmt(5, 5, 5, rgb, be);
                let expected_name = format!("{}555{}", if rgb { "rgb" } else { "bgr" }, if be { "be" } else { "le" });
                println!("testing roundtrip for {expected_name}");
                assert_eq!(fmt.to_short_string().unwrap(), expected_name);
                assert_eq!(NAPixelFormaton::from_str(&expected_name).unwrap(), fmt);

                let fmt = NAPixelFormaton::make_rgb16_fmt(5, 6, 5, rgb, be);
                let expected_name = format!("{}565{}", if rgb { "rgb" } else { "bgr" }, if be { "be" } else { "le" });
                println!("testing roundtrip for {expected_name}");
                assert_eq!(fmt.to_short_string().unwrap(), expected_name);
                assert_eq!(NAPixelFormaton::from_str(&expected_name).unwrap(), fmt);
            }
        }

        for &expected_name in ["rgba5551be", "bgra5551le"].iter() {
            println!("testing roundtrip for {expected_name}");
            let fmt = NAPixelFormaton::from_str(&expected_name).unwrap();
            assert_eq!(fmt.to_short_string().unwrap(), expected_name);
        }
    }
    #[test]
    fn test_yuv_fmt_names() {
        println!("formaton yuv420 - {}", YUV420_FORMAT);
        assert_eq!(YUV420_FORMAT.to_short_string().unwrap(), "yuv420p");
        assert_eq!(YUVA410_FORMAT.to_short_string().unwrap(), "yuva410p");

        let pfmt = NAPixelFormaton::from_str("yuv420").unwrap();
        println!("parsed pfmt as {} / {:?}", pfmt, pfmt.to_short_string());
        let pfmt = NAPixelFormaton::from_str("yuva420p12").unwrap();
        println!("parsed pfmt as {} / {:?}", pfmt, pfmt.to_short_string());

        for &fname in ["yuv410p", "yuv411p", "yuv420p", "yuv422p", "yuv440p", "yuv444p"].iter() {
            println!("trying to parse {fname}...");
            let fmt = NAPixelFormaton::from_str(fname).unwrap();
            let nname = fmt.to_short_string().unwrap();
            assert_eq!(fname, &nname);
        }
    }
}
