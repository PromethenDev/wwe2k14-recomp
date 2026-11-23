//! IMA ADPCM decoding functionality.

/// IMA ADPCM step change table.
pub const IMA_STEPS: [i8; 16] = [
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8
];

/// IMA ADPCM step size table.
pub const IMA_STEP_TABLE: [i32; 89] = [
        7,     8,     9,    10,    11,    12,    13,    14,
       16,    17,    19,    21,    23,    25,    28,    31,
       34,    37,    41,    45,    50,    55,    60,    66,
       73,    80,    88,    97,   107,   118,   130,   143,
      157,   173,   190,   209,   230,   253,   279,   307,
      337,   371,   408,   449,   494,   544,   598,   658,
      724,   796,   876,   963,  1060,  1166,  1282,  1411,
     1552,  1707,  1878,  2066,  2272,  2499,  2749,  3024,
     3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,
     7132,  7845,  8630,  9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
];

/// Maximum step value for IMA ADPCM.
pub const IMA_MAX_STEP: u8 = 88;

#[derive(Clone,Copy,Debug)]
/// Decoder for IMA ADPCM.
pub struct IMAState {
    /// Current sample value.
    pub predictor:  i32,
    /// Current step index.
    pub step:       usize,
}

impl IMAState {
    /// Constructs a new instance of `IMAState`.
    pub fn new() -> Self {
        Self {
            predictor:  0,
            step:       0,
        }
    }
    /// Re-initialises decoder with new predictor and step values.
    pub fn reset(&mut self, predictor: i16, step: u8) {
        self.predictor  = i32::from(predictor);
        self.step       = step.min(IMA_MAX_STEP) as usize;
    }
    /// Computes a new sample value from an input nibble.
    pub fn expand_sample(&mut self, nibble: u8) -> i16 {
        let istep = (self.step as isize) + (IMA_STEPS[(nibble & 0xF) as usize] as isize);
        let sign = (nibble & 8) != 0;
        let diff = (i32::from(2 * (nibble & 7) + 1) * IMA_STEP_TABLE[self.step]) >> 3;
        let sample = if !sign { self.predictor + diff } else { self.predictor - diff };
        self.predictor = sample.max(i32::from(i16::MIN)).min(i32::from(i16::MAX));
        self.step = istep.max(0).min(IMA_MAX_STEP as isize) as usize;
        self.predictor as i16
    }
    /// Computes an encoded nibble from an input sample.
    pub fn compress_sample(&self, sample: i16) -> u8 {
        let diff = i32::from(sample) - self.predictor;
        let sign = if diff >= 0 { 0 } else { 8 };
        let nib = (diff.abs() * 4 / IMA_STEP_TABLE[self.step]).min(7) as u8;
        nib | sign
    }
}

impl Default for IMAState {
    fn default() -> Self {
        Self::new()
    }
}
