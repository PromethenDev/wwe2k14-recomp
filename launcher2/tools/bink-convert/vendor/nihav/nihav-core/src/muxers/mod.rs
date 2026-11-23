//! Muxer definitions.
pub use crate::frame::*;
pub use crate::io::byteio::*;
pub use crate::demuxers::{StreamManager, StreamIter};
pub use crate::options::*;

/// A list specifying general muxing errors.
#[derive(Debug,Clone,Copy,PartialEq)]
#[allow(dead_code)]
pub enum MuxerError {
    /// An invalid argument was provided to the muxer.
    InvalidArgument,
    /// Trying to mux data without header being written.
    NotCreated,
    /// Muxer encountered invalid input packet.
    InvalidData,
    /// Input stream cannot be stored in this container format.
    UnsupportedFormat,
    /// Data writing error.
    IOError,
    /// Feature is not implemented.
    NotImplemented,
    /// Allocation failed.
    MemoryError,
    /// Operation cannot succeed in principle (e.g. seeking in an output stream not supporting seeking).
    NotPossible,
}

/// A specialised `Result` type for muxing operations.
pub type MuxerResult<T> = Result<T, MuxerError>;

/// Muxer capabilities.
#[derive(Clone,Copy,Debug,PartialEq)]
pub enum MuxerCapabilities {
    /// Muxer accepts single video stream with certain codec.
    ///
    /// Codec name `"any"` means various codecs are supported.
    SingleVideo(&'static str),
    /// Muxer accepts single audio stream with certain codec.
    ///
    /// Codec name `"any"` means various codecs are supported.
    SingleAudio(&'static str),
    /// Muxer accepts single video stream and single audio stream with defined codecs.
    SingleVideoAndAudio(&'static str, &'static str),
    /// Muxer accepts only video streams but can mux several video streams.
    OnlyVideo,
    /// Muxer accepts only audio streams but can mux several video streams..
    OnlyAudio,
    /// Muxer accepts variable amount of streams of any type.
    Universal,
}

impl From<ByteIOError> for MuxerError {
    fn from(_: ByteIOError) -> Self { MuxerError::IOError }
}

/// Muxer quirks (i.e. peculiarities in muxer behaviour like accepting only frames of fixed duration)
#[derive(Clone,Copy,Default)]
pub struct MuxerQuirks(pub u32);

/// Muxer expects all packets of one stream to have the same duration.
pub const MUX_QUIRK_FIXED_RATE: u32         = 1 <<  0;
/// Muxer expects to know full duration in advance.
pub const MUX_QUIRK_FIXED_DURATION: u32     = 1 <<  1;
/// Muxer does not care about synchronisation of the stream.
pub const MUX_QUIRK_UNSYNC: u32             = 1 <<  2;
/// Muxer does not support palette changes.
pub const MUX_QUIRK_GLOBAL_PAL: u32         = 1 <<  3;

impl MuxerQuirks {
    /// Creates a new instance of `MuxerQuirks`
    pub fn new() -> Self { Self::default() }

    /// Queries if the muxer expects all packets of one stream to have the same duration.
    pub fn is_fixed_rate(self) -> bool { (self.0 & MUX_QUIRK_FIXED_RATE) != 0 }
    /// Tells that muxer expects all packets of one stream to have the same duration.
    pub fn set_fixed_rate(&mut self) { self.0 |= MUX_QUIRK_FIXED_RATE; }
    /// Tells that muxer does not expect all packets of one stream to have the same duration.
    pub fn clear_fixed_rate(&mut self) { self.0 &= !MUX_QUIRK_FIXED_RATE; }

    /// Queries if the muxer expects to know full duration in advance.
    pub fn is_fixed_duration(self) -> bool { (self.0 & MUX_QUIRK_FIXED_DURATION) != 0 }
    /// Tells that muxer expects to know full duration in advance.
    pub fn set_fixed_duration(&mut self) { self.0 |= MUX_QUIRK_FIXED_DURATION; }
    /// Tells that muxer does not expect to know full duration in advance.
    pub fn clear_fixed_duration(&mut self) { self.0 &= !MUX_QUIRK_FIXED_DURATION; }

    /// Queries if the muxer does not care about stream synchronisation.
    pub fn is_unsync(self) -> bool { (self.0 & MUX_QUIRK_UNSYNC) != 0 }
    /// Tells that muxer does not care about stream synchronisation.
    pub fn set_unsync(&mut self) { self.0 |= MUX_QUIRK_UNSYNC; }
    /// Tells that muxer actually cares about streams being synchronised.
    pub fn clear_unsync(&mut self) { self.0 &= !MUX_QUIRK_UNSYNC; }

    /// Queries if the muxer does not support palette changes.
    pub fn is_global_pal(self) -> bool { (self.0 & MUX_QUIRK_GLOBAL_PAL) != 0 }
    /// Tells that muxer does not support palette changes.
    pub fn set_global_pal(&mut self) { self.0 |= MUX_QUIRK_GLOBAL_PAL; }
    /// Tells that muxer does not require global palette (and palette changes are fine).
    pub fn clear_global_pal(&mut self) { self.0 &= !MUX_QUIRK_GLOBAL_PAL; }
}


/// A trait for muxing operations.
pub trait MuxCore<'a>: NAOptionHandler {
    /// Prepares everything for packet muxing.
    fn create(&mut self, strmgr: &StreamManager) -> MuxerResult<()>;
    /// Queues a packet for muxing.
    fn mux_frame(&mut self, strmgr: &StreamManager, pkt: NAPacket) -> MuxerResult<()>;
    /// Flushes the current muxing state.
    fn flush(&mut self) -> MuxerResult<()>;
    /// Finishes muxing and writes necessary header and trailer information if needed.
    fn end(&mut self) -> MuxerResult<()>;
}

/// Muxer structure with auxiliary data.
pub struct Muxer<'a> {
    pub mux:        Box<dyn MuxCore<'a> + 'a>,
    pub streams:    StreamManager,
}

impl<'a> Muxer<'a> {
    /// Constructs a new `Muxer` instance.
    pub fn new(mux: Box<dyn MuxCore<'a> + 'a>, strmgr: StreamManager) -> Self {
        Muxer {
            mux,
            streams:    strmgr,
        }
    }
    /// Returns a stream reference by its number.
    pub fn get_stream(&self, idx: usize) -> Option<NAStreamRef> {
        self.streams.get_stream(idx)
    }
    /// Returns a stream reference by its ID.
    pub fn get_stream_by_id(&self, id: u32) -> Option<NAStreamRef> {
        self.streams.get_stream_by_id(id)
    }
    /// Reports the total number of streams.
    pub fn get_num_streams(&self) -> usize {
        self.streams.get_num_streams()
    }
    /// Returns an iterator over streams.
    pub fn get_streams(&self) -> StreamIter<'_> {
        self.streams.iter()
    }

    /// Queues a new packet for muxing.
    pub fn mux_frame(&mut self, pkt: NAPacket) -> MuxerResult<()> {
        self.mux.mux_frame(&self.streams, pkt)
    }
    /// Flushes the current muxing state.
    pub fn flush(&mut self) -> MuxerResult<()> {
        self.mux.flush()
    }
    /// Finishes muxing and writes necessary header and trailer information if needed.
    pub fn end(mut self) -> MuxerResult<()> {
        self.mux.end()
    }
}

impl<'a> NAOptionHandler for Muxer<'a> {
    fn get_supported_options(&self) -> &[NAOptionDefinition] {
        self.mux.get_supported_options()
    }
    fn set_options(&mut self, options: &[NAOption]) {
        self.mux.set_options(options);
    }
    fn query_option_value(&self, name: &str) -> Option<NAValue> {
        self.mux.query_option_value(name)
    }
}

/// The trait for creating muxers.
pub trait MuxerCreator {
    /// Creates new muxer instance that will use an object with `ByteIO` trait for output.
    fn new_muxer<'a>(&self, bw: &'a mut dyn ByteIO) -> Box<dyn MuxCore<'a> + 'a>;
    /// Returns the name of current muxer creator (equal to the container name it can create).
    fn get_name(&self) -> &'static str;
    /// Returns muxer capabilities for the current muxer.
    fn get_capabilities(&self) -> MuxerCapabilities;
    /// Returns current muxer quirks.
    fn get_quirks(&self) -> MuxerQuirks;
}

/// Creates muxer for a provided bytestream writer.
pub fn create_muxer<'a>(mxcr: &dyn MuxerCreator, strmgr: StreamManager, bw: &'a mut dyn ByteIO) -> MuxerResult<Muxer<'a>> {
    let mut mux = mxcr.new_muxer(bw);
    mux.create(&strmgr)?;
    Ok(Muxer::new(mux, strmgr))
}

/// List of registered muxers.
#[derive(Default)]
pub struct RegisteredMuxers {
    muxes:  Vec<&'static dyn MuxerCreator>,
}

impl RegisteredMuxers {
    /// Constructs a new `RegisteredMuxers` instance.
    pub fn new() -> Self {
        Self { muxes: Vec::new() }
    }
    /// Registers a new muxer.
    pub fn add_muxer(&mut self, mux: &'static dyn MuxerCreator) {
        self.muxes.push(mux);
    }
    /// Searches for a muxer that supports requested container format.
    pub fn find_muxer(&self, name: &str) -> Option<&dyn MuxerCreator> {
        self.muxes.iter().find(|&&mux| mux.get_name() == name).copied()
    }
    /// Provides an iterator over currently registered muxers.
    pub fn iter(&self) -> std::slice::Iter<'_, &dyn MuxerCreator> {
        self.muxes.iter()
    }
}
