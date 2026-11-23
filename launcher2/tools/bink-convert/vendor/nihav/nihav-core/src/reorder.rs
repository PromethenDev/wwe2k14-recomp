//! Output frame reordering.
//!
//! NihAV decoders output frames in the same order as they are put in.
//! In result if you want to have frames in display order you might need some frame reorderer.
//! This module provides such functionality depending on codec type: audio codecs and video codecs without B-frames do not need any reorderer and can use `NoReorderer` if the common interface is required. Codecs with B-frames should use `IPBReorderer`. For codecs with very complex reordering rules like H.264 or H.256 `PictureIDReorderer` will be added eventually.
//!
//! You can find out required reorderer by quering codec properties using `nihav_core::register` module.
use std::mem::swap;
use std::collections::VecDeque;
pub use crate::frame::{FrameType, NAFrameRef};

/// A trait for frame reorderer.
pub trait FrameReorderer {
    /// Stores a newly decoded frame.
    fn add_frame(&mut self, fref: NAFrameRef) -> bool;
    /// Gets the next frame to be displayed (or `None` if that is not possible).
    fn get_frame(&mut self) -> Option<NAFrameRef>;
    /// Clears all stored frames.
    fn flush(&mut self);
    /// Retrieves the last frames stored by the reorderer.
    fn get_last_frames(&mut self) -> Option<NAFrameRef>;
}

/// Zero reorderer.
pub struct NoReorderer {
    fref:   Option<NAFrameRef>,
}

impl NoReorderer {
    /// Constructs a new instance of `NoReorderer`.
    pub fn new() -> Self {
        Self { fref: None }
    }
}

impl Default for NoReorderer {
    fn default() -> Self {
        Self::new()
    }
}

impl FrameReorderer for NoReorderer {
    fn add_frame(&mut self, fref: NAFrameRef) -> bool {
        if self.fref.is_none() {
            self.fref = Some(fref);
            true
        } else {
            false
        }
    }
    fn get_frame(&mut self) -> Option<NAFrameRef> {
        let mut ret = None;
        swap(&mut ret, &mut self.fref);
        ret
    }
    fn flush(&mut self) { self.fref = None; }
    fn get_last_frames(&mut self) -> Option<NAFrameRef> { None }
}

/// Frame reorderer for codecs with I/P/B frames.
#[derive(Default)]
pub struct IPBReorderer {
    rframe:     Option<NAFrameRef>,
    bframe:     Option<NAFrameRef>,
}

impl IPBReorderer {
    /// Constructs a new instance of `IPBReorderer`.
    pub fn new() -> Self { Self::default() }
}

impl FrameReorderer for IPBReorderer {
    fn add_frame(&mut self, fref: NAFrameRef) -> bool {
        if self.rframe.is_some() && self.bframe.is_some() { return false; }
        let is_b = fref.get_frame_type() == FrameType::B;
        if is_b && self.bframe.is_some() { return false; }
        if is_b {
            self.bframe = Some(fref);
        } else {
            std::mem::swap(&mut self.bframe, &mut self.rframe);
            self.rframe = Some(fref);
        }
        true
    }
    fn get_frame(&mut self) -> Option<NAFrameRef> {
        let mut ret = None;
        if self.bframe.is_some() {
            std::mem::swap(&mut ret, &mut self.bframe);
        }
        ret
    }
    fn flush(&mut self) {
        self.rframe = None;
        self.bframe = None;
    }
    fn get_last_frames(&mut self) -> Option<NAFrameRef> {
        let mut ret = None;
        if self.bframe.is_some() {
            std::mem::swap(&mut ret, &mut self.bframe);
        } else if self.rframe.is_some() {
            std::mem::swap(&mut ret, &mut self.rframe);
        }
        ret
    }
}

/// Frame reorderer for codecs with complex I/P/B frame structure like ITU H.26x.
#[derive(Default)]
pub struct ComplexReorderer {
    last_ref_pts:   Option<u64>,
    ready_idx:      usize,
    frames:         Vec<NAFrameRef>,
}

impl ComplexReorderer {
    /// Constructs a new instance of `IPBReorderer`.
    pub fn new() -> Self { Self::default() }
}

impl FrameReorderer for ComplexReorderer {
    fn add_frame(&mut self, fref: NAFrameRef) -> bool {
        if self.frames.len() >= 64 {
            return false;
        }
        let is_ref = fref.frame_type == FrameType::I || fref.frame_type == FrameType::P;
        if !is_ref {
            if self.frames.is_empty() || fref.get_pts().is_none() {
                self.frames.push(fref);
            } else if let Some(new_pts) = fref.get_pts() {
                let mut idx = 0;
                for (i, frm) in self.frames.iter().enumerate() {
                    idx = i;
                    if let Some(pts) = frm.get_pts() {
                        if pts > new_pts {
                            break;
                        }
                    }
                }
                self.frames.insert(idx, fref);
            }
        } else {
            for (i, frm) in self.frames.iter().enumerate() {
                if frm.get_pts() == self.last_ref_pts {
                    self.ready_idx = i + 1;
                }
            }
            self.last_ref_pts = fref.get_pts();
            self.frames.push(fref);
        }
        true
    }
    fn get_frame(&mut self) -> Option<NAFrameRef> {
        if self.ready_idx > 0 {
            self.ready_idx -= 1;
            Some(self.frames.remove(0))
        } else {
            None
        }
    }
    fn flush(&mut self) {
        self.last_ref_pts = None;
        self.ready_idx = 0;
        self.frames.clear();
    }
    fn get_last_frames(&mut self) -> Option<NAFrameRef> {
        if !self.frames.is_empty() {
            Some(self.frames.remove(0))
        } else {
            None
        }
    }
}

/// A generic reorderer for a multi-threaded decoder.
#[derive(Default)]
pub struct MTFrameReorderer {
    ids:            Vec<u32>,
    frames:         VecDeque<(u32, NAFrameRef)>,
    cur_id:         u32,
    flush_mode:     bool,
    output_to:      Option<u32>,
    last_ts:        Option<u64>,
}

const MT_REORDER_DEPTH: usize = 16;

impl MTFrameReorderer {
    /// Constructs a new instance of `MTFrameReorderer`.
    pub fn new() -> Self { Self::default() }
    /// Registers the fact that a new frame is queued for decoding and returns an internal ID for it.
    pub fn register_frame(&mut self) -> u32 {
        if self.flush_mode {
            self.flush_mode = false;
        }
        let ret = self.cur_id;
        self.cur_id += 1;
        self.ids.push(ret);
        ret
    }
    /// Puts a newly decoded frame into the internal queue.
    pub fn add_frame(&mut self, frm: NAFrameRef, id: u32) {
        //let ftype = frm.get_frame_type();
        let frm_id = if let Some(ts) = frm.ts.pts { ts } else { u64::from(id) };
        let mut idx = 0;
        for (_, frm) in self.frames.iter() {
            let cur_id = if let Some(ts) = frm.ts.pts { ts } else { frm.id as u64 };
            if frm_id < cur_id {
                break;
            }
            idx += 1;
        }
        self.frames.insert(idx, (id, frm));
        /*if self.frames.len() > 48 {
            for (id, frm) in self.frames.iter() { print!(" {}{}({})", frm.get_frame_type(), frm.get_dts().unwrap_or(0), *id); } println!();
            print!("reg IDs:"); for &id in self.ids.iter() { print!(" {}", id); } println!();
            panic!("too many frames in the queue");
        }*/
    }
    /// Removes the registered frame (e.g. in case of a decoding error).
    pub fn drop_frame(&mut self, id: u32) {
        self.ids.retain(|&el| el != id);
    }
    fn get_first_frame(&mut self) -> Option<NAFrameRef> {
        let (id, frm) = self.frames.pop_front().unwrap();
        self.drop_frame(id);
        self.last_ts = frm.get_pts();
        Some(frm)
    }
    /// Gets the next frame to be displayed (or `None` if that is not possible).
    pub fn get_frame(&mut self) -> Option<NAFrameRef> {
        if self.frames.is_empty() {
            return None;
        }
        // check if we have consequent timestamps that we can output
        if let Some(pts) = self.frames[0].1.get_pts() {
            let next_ts = self.last_ts.map(|val| val + 1);
            if (self.last_ts.is_none() && pts == 0) || (Some(pts) == next_ts) {
                self.output_to = None;
                return self.get_first_frame();
            }
        }
        if !self.flush_mode {
            if self.ids.len() < MT_REORDER_DEPTH {
                return None;
            }
            for &id in self.ids.iter().take(MT_REORDER_DEPTH) {
                if !self.frames.iter().any(|frm| frm.0 == id) {
                    return None;
                }
            }
            self.get_first_frame()
        } else {
            Some(self.frames.pop_front().unwrap().1)
        }
    }
    /// Retrieves the last frames stored by the reorderer.
    pub fn get_last_frames(&mut self) -> Option<NAFrameRef> {
        self.flush_mode = true;
        self.get_frame()
    }
    /// Clears all stored frames.
    pub fn flush(&mut self) {
        self.flush_mode = false;
        self.frames.clear();
        self.ids.clear();
        self.output_to = None;
        self.last_ts = None;
    }
}
