use super::Pixel;

pub struct NeuQuantQuantiser {
    weights:    [[f64; 3]; 256],
    freq:       [f64; 256],
    bias:       [f64; 256],
    factor:     usize,
}

const SPECIAL_NODES: usize = 2;
impl NeuQuantQuantiser {
    #[allow(clippy::needless_range_loop)]
    pub fn new(factor: usize) -> Self {
        let mut weights = [[0.0; 3]; 256];
        if SPECIAL_NODES > 1 {
            weights[1] = [255.0; 3]; // for white
        }
        for i in SPECIAL_NODES..256 {
            let w = 255.0 * ((i - SPECIAL_NODES) as f64) / ((256 - SPECIAL_NODES) as f64);
            weights[i] = [w, w, w];
        }
        Self {
            weights,
            freq:       [1.0 / 256.0; 256],
            bias:       [0.0; 256],
            factor,
        }
    }
    fn update_node(&mut self, idx: usize, clr: &[f64; 3], alpha: f64) {
        self.weights[idx][0] -= alpha * (self.weights[idx][0] - clr[0]);
        self.weights[idx][1] -= alpha * (self.weights[idx][1] - clr[1]);
        self.weights[idx][2] -= alpha * (self.weights[idx][2] - clr[2]);
    }
    fn update_neighbours(&mut self, idx: usize, clr: &[f64; 3], alpha: f64, radius: usize) {
        let low  = idx.saturating_sub(radius).max(SPECIAL_NODES - 1);
        let high = (idx + radius).min(self.weights.len() - 1);

        let mut idx0 = idx + 1;
        let mut idx1 = idx - 1;
        let mut range = 0;
        let sqradius = (radius * radius) as f64;
        while (idx0 < high) || (idx1 > low) {
            let sqrng = f64::from(range * range);
            let a = alpha * (sqradius - sqrng) / sqradius;
            range += 1;
            if idx0 < high {
                self.update_node(idx0, clr, a);
                idx0 += 1;
            }
            if idx1 > low {
                self.update_node(idx1, clr, a);
                idx1 -= 1;
            }
        }
    }
    #[allow(clippy::float_cmp)]
    fn find_node(&mut self, clr: &[f64; 3]) -> usize {
        for i in 0..SPECIAL_NODES {
            if &self.weights[i] == clr {
                return i;
            }
        }
        let mut bestdist = f64::MAX;
        let mut distidx = 0;
        let mut bestbias = f64::MAX;
        let mut biasidx = 0;
        for i in SPECIAL_NODES..256 {
            let dist = (self.weights[i][0] - clr[0]) * (self.weights[i][0] - clr[0])
                     + (self.weights[i][1] - clr[1]) * (self.weights[i][1] - clr[1])
                     + (self.weights[i][2] - clr[2]) * (self.weights[i][2] - clr[2]);
            if bestdist > dist {
                bestdist = dist;
                distidx = i;
            }
            let biasdiff = dist - self.bias[i];
            if bestbias > biasdiff {
                bestbias = biasdiff;
                biasidx = i;
            }
            self.freq[i] -= self.freq[i] / 1024.0;
            self.bias[i] += self.freq[i];
        }
        self.freq[distidx] += 1.0 / 1024.0;
        self.bias[distidx] -= 1.0;
        biasidx
    }
    pub fn learn(&mut self, src: &[Pixel]) {
        let mut bias_radius = (256 / 8) << 6;
        let alphadec = (30 + (self.factor - 1) / 3) as f64;
        let initial_alpha = f64::from(1 << 10);

        let npixels = src.len();

        let mut radius = bias_radius >> 6;
        if radius == 1 { radius = 0 };
        let samples = npixels / self.factor;
        let delta = samples / 100;
        let mut alpha = initial_alpha;

        let mut pos = 0;
        const PRIMES: [usize; 4] = [ 499, 491, 487, 503 ];
        let mut step = PRIMES[3];
        for prime in PRIMES.iter().rev() {
            if npixels % *prime != 0 {
                step = *prime;
            }
        }

        for i in 0..samples {
            let clr = [f64::from(src[pos].r), f64::from(src[pos].g), f64::from(src[pos].b)];
            let idx = self.find_node(&clr);
            if idx >= SPECIAL_NODES {
                let new_alpha = alphadec / initial_alpha;
                self.update_node(idx, &clr, new_alpha);
                if radius > 0 {
                    self.update_neighbours(idx, &clr, new_alpha, radius);
                }
            }
            pos = (pos + step) % npixels;
            if (i + 1) % delta == 0 {
                alpha -= alpha / alphadec;
                bias_radius -= bias_radius / 30;
                radius = bias_radius >> 6;
                if radius == 1 { radius = 0 };
            }
        }
    }
    pub fn make_pal(&self, pal: &mut [[u8; 3]; 256]) {
        for (pal, node) in pal.iter_mut().zip(self.weights.iter()) {
            pal[0] = (node[0] + 0.5).clamp(0.0, 255.0) as u8;
            pal[1] = (node[1] + 0.5).clamp(0.0, 255.0) as u8;
            pal[2] = (node[2] + 0.5).clamp(0.0, 255.0) as u8;
        }
    }
}
