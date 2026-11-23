use super::BlockMode;

#[derive(Default)]
pub struct RateControl {
    bitrate:    u32,
    bitpool:    u32,
    tb_num:     u32,
    tb_den:     u32,
    fpos:       u32,
    quality:    u8,
    lambda:     f32,
    first:      bool,
}

impl RateControl {
    pub fn new() -> Self {
        Self {
            lambda: 1.0,
            ..Default::default()
        }
    }
    pub fn init(&mut self, tb_num: u32, tb_den: u32, bitrate: u32, quality: u8) {
        self.tb_num  = tb_num;
        self.tb_den  = tb_den;
        self.bitrate = bitrate;
        self.quality = quality;

        self.bitpool = self.bitrate;
        self.fpos = 0;
        self.first = true;
    }
    pub fn metric(&self, diff: u32, bits: usize) -> u32 {
        diff.saturating_add((self.get_weight() * (bits as f32)) as u32)
    }
    fn get_weight(&self) -> f32 {
        if (0..=100).contains(&self.quality) {
            self.lambda * ((100 - self.quality) as f32)
        } else {
            self.lambda
        }
    }
    pub fn expected_size(&self) -> u32 {
        if self.bitrate != 0 {
            (if !self.first {
                let ticks = self.tb_den - self.fpos;
                u64::from(self.bitpool) * u64::from(self.tb_num) / u64::from(ticks)
            } else {
                u64::from(self.bitrate) * 4 * u64::from(self.tb_num) / u64::from(self.tb_den)
            }) as u32
        } else {
            0
        }
    }
    pub fn update_size(&mut self, real_size: usize) {
        if self.bitrate != 0 {
            let bits = (real_size * 8) as u32;
            let tgt_size = self.expected_size();

            self.fpos += self.tb_num;
            while self.fpos >= self.tb_den {
                self.fpos -= self.tb_den;
                self.bitpool += self.bitrate;
            }
            self.bitpool = self.bitpool.saturating_sub(bits);

            if bits > tgt_size + tgt_size / 8 {
                self.lambda += 0.1;
            }
            if bits < tgt_size - tgt_size / 8 {
                self.lambda -= 0.1;
                if self.lambda < 0.0 {
                    self.lambda = 0.0;
                }
            }
            self.first = false;
        }
    }
    pub fn pattern_run_threshold(&self) -> u8 {
        match self.quality {
            1..=39 => 4,
            40..=59 => 3,
            60..=79 => 2,
            80..=89 => 1,
            _ => 0,
        }
    }
    pub fn get_quant_ranges(&self) -> [u8; 4] {
        match self.quality {
            98..=100 => [  0,  0,  0,  2 ],
            92..=97  => [  2, 16,  4, 16 ],
            85..=91  => [  5, 16,  7, 16 ],
            75..=84  => [  8, 16, 10, 16 ],
            55..=74  => [ 11, 16, 12, 16 ],
             1..=54  => [ 12, 16, 13, 16 ],
            _        => [  0, 16,  0, 16 ],
        }
    }
    pub fn modify_forbidden_btypes(&self, forbidden: &mut [bool; 12]) {
        if self.quality > 98 {
            forbidden[usize::from(BlockMode::Intra)] = true;
        }
        if self.quality > 0 {
            if self.quality < 80 {
                forbidden[usize::from(BlockMode::Run)] = true;
                forbidden[usize::from(BlockMode::Residue)] = true;
            }
            if self.quality < 90 {
                forbidden[usize::from(BlockMode::Raw)] = true;
            }
        }
    }
}
