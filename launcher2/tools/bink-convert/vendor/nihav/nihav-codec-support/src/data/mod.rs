//! Auxiliary data structures for codecs.

/// Line cache.
///
/// In the decoding process of many codecs there is a need to store some previously decoded information and only immediate top neighbours are used.
/// This can be done by storing either the full information for the whole frame or just the top line and move information for last decoded row to the top every time when row decoding is done.
/// `GenericCache` implements the second approach.
///
/// # Examples
///
/// Create a cache for one line and use top pixel for prediction:
/// ```
/// use nihav_codec_support::data::GenericCache;
///
/// # let width = 640;
/// # let height = 480;
/// # let mut dst: Vec<u8> = vec![0; width];
/// let mut linecache: GenericCache<u8> = GenericCache::new(1, width, 0x80);
/// for _ in 0..height {
///     for x in 0..width {
///         # let delta = 32;
///         dst[x] = linecache.data[linecache.xpos + x - linecache.stride] + delta;
///     }
///     linecache.update_row();
/// }
/// ```
pub struct GenericCache<T: Copy> {
    /// Number of rows that are processed at once.
    pub height: usize,
    /// Number of elements in one row.
    pub stride: usize,
    /// Start of curent data.
    pub xpos:   usize,
    /// Data.
    pub data:   Vec<T>,
    /// Default value to fill the cache with.
    pub default: T,
}

impl<T:Copy> GenericCache<T> {
    /// Constructs a new instance of `GenericCache`.
    pub fn new(height: usize, stride: usize, default: T) -> Self {
        let mut ret = Self {
                stride,
                height,
                xpos:   0,
                data:   Vec::with_capacity((height + 1) * stride + 1),
                default,
            };
        ret.reset();
        ret
    }
    /// Reports the total amount of elements stored.
    pub fn full_size(&self) -> usize { self.stride * (self.height + 1) + 1 }
    /// Resets the cache state.
    pub fn reset(&mut self) {
        self.data.clear();
        let size = self.full_size();
        self.data.resize(size, self.default);
        self.xpos = self.stride + 1;
    }
    /// Updates cache state for the next line.
    pub fn update_row(&mut self) {
        for i in 0..self.stride {
            self.data[i] = self.data[self.height * self.stride + i];
        }
        self.data.truncate(self.stride);
        let size = self.full_size();
        self.data.resize(size, self.default);
        self.xpos = self.stride + 1;
    }
}

