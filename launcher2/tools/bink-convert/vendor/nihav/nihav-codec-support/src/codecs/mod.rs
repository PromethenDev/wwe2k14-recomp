//! Decoder support functions and definitions.
use std::fmt;
use std::ops::{Add, AddAssign, Sub, SubAssign, Neg};

pub use nihav_core::frame::*;
use std::mem;

/// Frame manager for hold-and-modify codecs.
///
/// This frame manager simplifies frame management for the case when codec decodes new frame by updating parts of the previous frame.
///
/// # Examples
///
/// ```ignore
/// let mut frame = if is_intra_frame {
///         allocate_video_frame()
///     } else {
///         let ret = shuffler.clone_ref();
///         if ret.is_none() {
///             return Err(DecoderError::MissingReference);
///         }
///         ret.unwrap()
///     };
/// // output data into the frame
/// shuffler.add_frame(frame.clone()); // tells frame manager to use the frame as the next reference
/// ```
#[allow(dead_code)]
#[derive(Default)]
pub struct HAMShuffler<T: Copy> {
    lastframe: Option<NAVideoBufferRef<T>>,
}

impl<T: Copy> HAMShuffler<T> {
    /// Constructs a new instance of frame manager.
    #[allow(dead_code)]
    pub fn new() -> Self { HAMShuffler { lastframe: None } }
    /// Clears the reference.
    #[allow(dead_code)]
    pub fn clear(&mut self) { self.lastframe = None; }
    /// Sets a new frame reference.
    #[allow(dead_code)]
    pub fn add_frame(&mut self, buf: NAVideoBufferRef<T>) {
        self.lastframe = Some(buf);
    }
    /// Provides a copy of the reference frame if present or `None` if it is not.
    #[allow(dead_code)]
    pub fn clone_ref(&mut self) -> Option<NAVideoBufferRef<T>> {
        if let Some(ref mut frm) = self.lastframe {
            let newfrm = frm.copy_buffer();
            *frm = newfrm.clone().into_ref();
            Some(newfrm.into_ref())
        } else {
            None
        }
    }
    /// Returns the original saved reference frame or `None` if it is not present.
    #[allow(dead_code)]
    pub fn get_output_frame(&mut self) -> Option<NAVideoBufferRef<T>> {
        self.lastframe.as_ref().cloned()
    }
}

/// Frame manager for codecs with intra and inter frames.
///
/// This frame manager simplifies frame management for the case when codec decodes new frame using previous frame as source of some data.
///
/// # Examples
///
/// ```ignore
/// let mut frame = allocate_video_frame();
/// if is_inter_frame {
///     let ret = shuffler.get_ref();
///     if ret.is_none() {
///         return Err(DecoderError::MissingReference);
///     }
///     let ref_frame = ret.unwrap();
///     // keep decoding using data from ref_frame
/// }
/// shuffler.add_frame(frame.clone()); // tells frame manager to use the frame as the next reference
/// ```
#[allow(dead_code)]
#[derive(Default)]
pub struct IPShuffler {
    lastframe: Option<NAVideoBufferRef<u8>>,
}

impl IPShuffler {
    /// Constructs a new instance of frame manager.
    #[allow(dead_code)]
    pub fn new() -> Self { IPShuffler { lastframe: None } }
    /// Clears the reference.
    #[allow(dead_code)]
    pub fn clear(&mut self) { self.lastframe = None; }
    /// Sets a new frame reference.
    #[allow(dead_code)]
    pub fn add_frame(&mut self, buf: NAVideoBufferRef<u8>) {
        self.lastframe = Some(buf);
    }
    /// Returns the original saved reference frame or `None` if it is not present.
    #[allow(dead_code)]
    pub fn get_ref(&mut self) -> Option<NAVideoBufferRef<u8>> {
        self.lastframe.as_ref().cloned()
    }
}

/// Frame manager for codecs with I-, P- and B-frames.
///
/// This frame manager simplifies frame management for the case when codec uses I/P/B frame scheme.
///
/// # Examples
///
/// ```ignore
/// let mut frame = allocate_video_frame();
/// for mb in all_macroblocks {
///     // decode macroblock type
///     match mb_type {
///         MBType::Inter => {
///             do_mc(&mut frame, shuffler.get_lastref().unwrap());
///         },
///         MBType::BForward => {
///             do_mc(&mut frame, shuffler.get_b_fwdref().unwrap());
///         },
///         MBType::BBackward => {
///             do_mc(&mut frame, shuffler.get_b_bwdref().unwrap());
///         },
///         // handle the rest of cases
///     };
/// if is_random_access_frame {
///     shuffler.clear(); // remove all saved references
/// }
/// if is_intra_frame || is_p_frame {
///     shuffler.add_frame(frame.clone()); // tells frame manager to use the frame as the next reference
/// }
/// ```
#[allow(dead_code)]
#[derive(Default)]
pub struct IPBShuffler {
    lastframe: Option<NAVideoBufferRef<u8>>,
    nextframe: Option<NAVideoBufferRef<u8>>,
}

impl IPBShuffler {
    /// Constructs a new instance of frame manager.
    #[allow(dead_code)]
    pub fn new() -> Self { IPBShuffler { lastframe: None, nextframe: None } }
    /// Clears the reference.
    #[allow(dead_code)]
    pub fn clear(&mut self) { self.lastframe = None; self.nextframe = None; }
    /// Sets a new frame reference.
    #[allow(dead_code)]
    pub fn add_frame(&mut self, buf: NAVideoBufferRef<u8>) {
        mem::swap(&mut self.lastframe, &mut self.nextframe);
        self.lastframe = Some(buf);
    }
    /// Returns the previous reference frame or `None` if it is not present.
    #[allow(dead_code)]
    pub fn get_lastref(&mut self) -> Option<NAVideoBufferRef<u8>> {
        self.lastframe.as_ref().cloned()
    }
    /// Returns second last reference frame or `None` if it is not present.
    #[allow(dead_code)]
    pub fn get_nextref(&mut self) -> Option<NAVideoBufferRef<u8>> {
        self.nextframe.as_ref().cloned()
    }
    /// Returns the temporally following reference for B-frame or `None` if it is not present.
    #[allow(dead_code)]
    pub fn get_b_fwdref(&mut self) -> Option<NAVideoBufferRef<u8>> {
        self.nextframe.as_ref().cloned()
    }
    /// Returns the temporally preceeding reference for B-frame or `None` if it is not present.
    #[allow(dead_code)]
    pub fn get_b_bwdref(&mut self) -> Option<NAVideoBufferRef<u8>> {
        self.lastframe.as_ref().cloned()
    }
}

/// Motion vector data type.
///
/// # Examples
///
/// ```
/// use nihav_codec_support::codecs::MV;
///
/// let mv0 = MV::new(1, 3);
/// let mv1 = MV { x: 2, y: 3 }; // choose whatever style you prefer
/// let mv2 = mv1 - mv0;
/// let mv_pred = MV::pred(mv0, mv1, mv2); // get median prediction for the vectors (1, 0)
/// ```
#[derive(Debug,Clone,Copy,Default,PartialEq)]
pub struct MV {
    /// X coordinate of the vector.
    pub x: i16,
    /// Y coordinate of the vector.
    pub y: i16,
}

#[allow(clippy::many_single_char_names)]
#[allow(clippy::collapsible_if)]
#[allow(clippy::collapsible_else_if)]
impl MV {
    /// Creates a new motion vector instance.
    pub fn new(x: i16, y: i16) -> Self { MV{ x, y } }
    /// Predicts median from provided motion vectors.
    ///
    /// Each component of the vector is predicted as the median of corresponding input vector components.
    pub fn pred(a: MV, b: MV, c: MV) -> Self {
        let x;
        if a.x < b.x {
            if b.x < c.x {
                x = b.x;
            } else {
                if a.x < c.x { x = c.x; } else { x = a.x; }
            }
        } else {
            if b.x < c.x {
                if a.x < c.x { x = a.x; } else { x = c.x; }
            } else {
                x = b.x;
            }
        }
        let y;
        if a.y < b.y {
            if b.y < c.y {
                y = b.y;
            } else {
                if a.y < c.y { y = c.y; } else { y = a.y; }
            }
        } else {
            if b.y < c.y {
                if a.y < c.y { y = a.y; } else { y = c.y; }
            } else {
                y = b.y;
            }
        }
        MV { x, y }
    }
}

/// Zero motion vector.
pub const ZERO_MV: MV = MV { x: 0, y: 0 };

impl Add for MV {
    type Output = MV;
    fn add(self, other: MV) -> MV { MV { x: self.x + other.x, y: self.y + other.y } }
}

impl AddAssign for MV {
    fn add_assign(&mut self, other: MV) { self.x += other.x; self.y += other.y; }
}

impl Sub for MV {
    type Output = MV;
    fn sub(self, other: MV) -> MV { MV { x: self.x - other.x, y: self.y - other.y } }
}

impl SubAssign for MV {
    fn sub_assign(&mut self, other: MV) { self.x -= other.x; self.y -= other.y; }
}

impl Neg for MV {
    type Output = MV;
    fn neg(self) -> Self::Output {
        MV { x: -self.x, y: -self.y }
    }
}

impl fmt::Display for MV {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        write!(f, "{},{}", self.x, self.y)
    }
}

#[cfg(feature="blockdsp")]
pub mod blockdsp;

#[cfg(feature="jpeg")]
pub mod jpeg;

#[cfg(feature="qt_pal")]
pub mod qt_pal;

#[cfg(feature="h263")]
#[allow(clippy::collapsible_if)]
#[allow(clippy::manual_memcpy)]
#[allow(clippy::needless_range_loop)]
pub mod h263;

/// The common 8x8 zigzag scan.
pub const ZIGZAG: [usize; 64] = [
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
];

pub mod imaadpcm;

pub mod msstructs;
