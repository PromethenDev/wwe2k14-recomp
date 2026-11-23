use std::f32::consts;

pub const MAX_BANDS: usize = 25;

const CRITICAL_FREQS: [usize; MAX_BANDS] = [
      100,   200,  300,  400,  510,  630,   770,   920,
     1080,  1270, 1480, 1720, 2000, 2320,  2700,  3150,
     3700,  4400, 5300, 6400, 7700, 9500, 12000, 15500,
    24500
];

pub const RUN_TAB: [usize; 16] = [ 2, 3, 4, 5, 6, 8, 9, 10, 11, 12, 13, 14, 15, 16, 32, 64 ];

pub fn init_bands(srate: usize, len: usize, num_bands: &mut usize, bands: &mut [usize; MAX_BANDS + 1]) {
    *num_bands = 1;
    while *num_bands < CRITICAL_FREQS.len() && CRITICAL_FREQS[*num_bands - 1] < srate {
        *num_bands += 1;
    }
    bands[0] = 2;
    for (band, &freq) in bands[1..*num_bands].iter_mut().zip(CRITICAL_FREQS.iter()) {
        *band = (freq * len / srate) & !1;
    }
    bands[*num_bands] = len;
}

pub fn get_quants_table() -> [f32; 96] {
    let mut quants: [f32; 96] = [0.0; 96];
    for (i, quant) in quants.iter_mut().enumerate() {
        *quant = ((i as f32) * 0.0664 / consts::LOG10_E).exp();
    }
    quants
}
