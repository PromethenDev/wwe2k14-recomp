use super::Pixel;

struct RNG {
    seed: u16,
}

impl RNG {
    fn new() -> Self { Self { seed: 0x1234 } }
    fn next(&mut self) -> u8 {
        if (self.seed & 0x8000) != 0 {
            self.seed = ((self.seed & 0x7FFF) * 2) ^ 0x1B2B;
        } else {
            self.seed <<= 1;
        }
        self.seed as u8
    }
}

#[derive(Default,Clone,Copy,PartialEq,Debug)]
struct Entry {
    pix:        Pixel,
    count:      u64,
}

struct Cluster {
    centroid:   Pixel,
    dist:       u64,
    count:      u64,
    sum_r:      u64,
    sum_g:      u64,
    sum_b:      u64,
}

impl Cluster {
    fn new(centroid: Pixel) -> Self {
        Self {
            centroid,
            dist:       0,
            count:      0,
            sum_r:      0,
            sum_g:      0,
            sum_b:      0,
        }
    }
    fn reset(&mut self) {
        self.count = 0;
        self.sum_r = 0;
        self.sum_g = 0;
        self.sum_b = 0;
        self.dist  = 0;
    }
    fn add_pixel(&mut self, entry: &Entry) {
        self.sum_r += u64::from(entry.pix.r) * entry.count;
        self.sum_g += u64::from(entry.pix.g) * entry.count;
        self.sum_b += u64::from(entry.pix.b) * entry.count;
        self.count += entry.count;
    }
    fn add_dist(&mut self, entry: &Entry) {
        self.dist += u64::from(self.centroid.dist(entry.pix)) * entry.count;
    }
    fn calc_centroid(&mut self) {
        if self.count != 0 {
            self.centroid.r = ((self.sum_r + self.count / 2) / self.count) as u8;
            self.centroid.g = ((self.sum_g + self.count / 2) / self.count) as u8;
            self.centroid.b = ((self.sum_b + self.count / 2) / self.count) as u8;
        }
    }
    fn calc_dist(&mut self) {
    }
}

pub struct ELBG {
    clusters:   Vec<Cluster>,
}

impl ELBG {
    #[allow(dead_code)]
    pub fn new(initial_pal: &[[u8; 3]; 256]) -> Self {
        let mut clusters = Vec::with_capacity(256);
        for clr in initial_pal.iter() {
            let pix = Pixel { r: clr[0], g: clr[1], b: clr[2] };
            let cluster = Cluster::new(pix);
            clusters.push(cluster);
        }
        Self {
            clusters,
        }
    }
    #[allow(dead_code)]
    pub fn new_random() -> Self {
        let mut rng = RNG::new();
        let mut clusters = Vec::with_capacity(256);
        for _ in 0..256 {
            let pix = Pixel { r: rng.next(), g: rng.next(), b: rng.next() };
            let cluster = Cluster::new(pix);
            clusters.push(cluster);
        }
        Self {
            clusters,
        }
    }
    fn sort<F>(arr: &mut [Pixel], idx: F) where F: Fn(&Pixel) -> u8 {
        let mut dst = vec![Pixel::default(); arr.len()];
        let mut counts = [0; 256];
        for pix in arr.iter() {
            counts[idx(pix) as usize] += 1;
        }
        let mut last = counts[0];
        counts[0] = 0;
        for count in counts.iter_mut().skip(1) {
            let plast = last;
            last += *count;
            *count = plast;
        }
        for pix in arr.iter() {
            let bucket = idx(pix) as usize;
            dst[counts[bucket]] = *pix;
            counts[bucket] += 1;
        }
        arr.copy_from_slice(dst.as_slice());
    }
    fn new_split(old_index: usize, entries: &[Entry], indices: &[usize]) -> Option<(Pixel, Pixel)> {
        let mut max = Pixel { r:   0, g:   0, b:   0 };
        let mut min = Pixel { r: 255, g: 255, b: 255 };
        let mut found = false;
        for (entry, idx) in entries.iter().zip(indices) {
            if *idx == old_index {
                max = max.max(entry.pix);
                min = min.min(entry.pix);
                found = true;
            }
        }
        if !found {
            return None;
        }
        let dr = max.r - min.r;
        let dg = max.g - min.g;
        let db = max.b - min.b;
        let cent0 = Pixel { r: min.r + dr / 3, g: min.g + dg / 3, b: min.b + db / 3 };
        let cent1 = Pixel { r: max.r - dr / 3, g: max.g - dg / 3, b: max.b - db / 3 };
        Some((cent0, cent1))
    }
    fn old_centre(&self, old_index1: usize, old_index2: usize, entries: &[Entry], indices: &[usize]) -> Pixel {
        let mut max = Pixel { r:   0, g:   0, b:   0 };
        let mut min = Pixel { r: 255, g: 255, b: 255 };
        let mut found = false;
        for (entry, idx) in entries.iter().zip(indices) {
            if *idx == old_index1 || *idx == old_index2 {
                max = max.max(entry.pix);
                min = min.min(entry.pix);
                found = true;
            }
        }
        if !found {
            max = self.clusters[old_index1].centroid.max(self.clusters[old_index2].centroid);
            min = self.clusters[old_index1].centroid.min(self.clusters[old_index2].centroid);
        }
        let dr = max.r - min.r;
        let dg = max.g - min.g;
        let db = max.b - min.b;
        Pixel { r: min.r + dr / 2, g: min.g + dg / 2, b: min.b + db / 2 }
    }
    fn estimate_old(old_idx0: usize, old_idx1: usize, c: Pixel, entries: &[Entry], indices: &[usize]) -> u64 {
        let mut clu = Cluster::new(c);
        let mut count = 0;
        for (entry, idx) in entries.iter().zip(indices) {
            if *idx == old_idx0 || *idx == old_idx1 {
                clu.add_dist(entry);
                count += entry.count;
            }
        }
        clu.count = count;
        clu.calc_dist();
        clu.dist
    }
    fn estimate_new(c0: Pixel, c1: Pixel, old_idx: usize, entries: &[Entry], indices: &[usize]) -> u64 {
        let mut clu0 = Cluster::new(c0);
        let mut clu1 = Cluster::new(c1);
        let mut count0 = 0;
        let mut count1 = 0;
        for (entry, idx) in entries.iter().zip(indices) {
            if *idx == old_idx {
                if c0.dist(entry.pix) < c1.dist(entry.pix) {
                    clu0.add_dist(entry);
                    count0 += entry.count;
                } else {
                    clu1.add_dist(entry);
                    count1 += entry.count;
                }
            }
        }
        clu0.count = count0;
        clu1.count = count1;
        clu0.calc_dist();
        clu1.calc_dist();
        clu0.dist + clu1.dist
    }
    #[allow(clippy::cognitive_complexity)]
    pub fn quantise(&mut self, src: &[Pixel], dst: &mut [[u8; 3]; 256]) {
        if src.len() < 3 {
            return;
        }
        let mut old_cb: [Pixel; 256] = [Pixel::default(); 256];
        let mut prev_dist = u64::MAX;
        let mut dist = u64::MAX / 2;
        let mut indices = Vec::with_capacity(src.len());
        let mut pixels = Vec::with_capacity(src.len());
        pixels.extend_from_slice(src);
        Self::sort(pixels.as_mut_slice(), |pix| pix.r);
        Self::sort(pixels.as_mut_slice(), |pix| pix.g);
        Self::sort(pixels.as_mut_slice(), |pix| pix.b);
        let mut entries = Vec::with_capacity(pixels.len() / 2);
        let mut lastval = pixels[0];
        let mut run = 1;
        for pix in pixels.iter().skip(1) {
            if &lastval == pix {
                run += 1;
            } else {
                entries.push(Entry { pix: lastval, count: run });
                lastval = *pix;
                run = 1;
            }
        }
        entries.push(Entry { pix: lastval, count: run });
        drop(pixels);

        let mut low_u:  Vec<usize> = Vec::with_capacity(256);
        let mut high_u: Vec<usize> = Vec::with_capacity(256);
        let mut rng = RNG::new();
        let mut iterations = 0usize;
        let mut do_elbg_step = true;
        while (iterations < 10) && (dist < prev_dist - prev_dist / 100) {
            prev_dist = dist;
            for (cb, cluster) in old_cb.iter_mut().zip(self.clusters.iter_mut()) {
                *cb = cluster.centroid;
                cluster.reset();
            }
            // put pixels into the nearest clusters
            indices.clear();
            for entry in entries.iter() {
                let mut bestidx = 0;
                let mut bestdist = u32::MAX;
                for (i, cluster) in self.clusters.iter().enumerate() {
                    let dist = entry.pix.dist(cluster.centroid);
                    if bestdist > dist {
                        bestdist = dist;
                        bestidx = i;
                        if dist == 0 {
                            break;
                        }
                    }
                }
                indices.push(bestidx);
                self.clusters[bestidx].add_pixel(entry);
            }
            // calculate params
            for cluster in self.clusters.iter_mut() {
                cluster.calc_centroid();
            }
            dist = 0;
            for (idx, entry) in indices.iter().zip(entries.iter()) {
                self.clusters[*idx].add_dist(entry);
            }
            for cluster in self.clusters.iter_mut() {
                cluster.calc_dist();
                dist += cluster.dist;
            }

            let dmean = dist / 256;
            low_u.clear();
            high_u.clear();
            let mut used = [false; 256];
            for (i, cluster) in self.clusters.iter().enumerate() {
                if cluster.dist < dmean {
                    low_u.push(i);
                } else if cluster.dist > dmean * 2 {
                    high_u.push(i);
                    used[i] = true;
                }
            }

            if do_elbg_step {
                do_elbg_step = false;
                for low_idx in low_u.iter() {
                    if high_u.is_empty() {
                        break;
                    }
                    let high_idx_idx = (rng.next() as usize) % high_u.len();
                    let high_idx = high_u[high_idx_idx];
                    let mut closest_idx = *low_idx;
                    let mut closest_dist = u32::MAX;
                    let low_centr = self.clusters[*low_idx].centroid;
                    for (i, (cluster, &used)) in self.clusters.iter().zip(used.iter()).enumerate() {//low_u.iter() {
                        if i == *low_idx || used {
                            continue;
                        }
                        let dist = cluster.centroid.dist(low_centr);
                        if closest_dist > dist {
                            closest_dist = dist;
                            closest_idx  = i;
                        }
                    }
                    if closest_idx == *low_idx {
                        continue;
                    }
                    let old_dist = self.clusters[*low_idx].dist + self.clusters[closest_idx].dist + self.clusters[high_idx].dist;
                    let old_centr = self.old_centre(*low_idx, closest_idx, entries.as_slice(), indices.as_slice());
                    let ret = Self::new_split(high_idx, entries.as_slice(), indices.as_slice());
                    if ret.is_none() {
                        continue;
                    }
                    let (centr0, centr1) = ret.unwrap();
                    let dist_o = if old_dist > self.clusters[high_idx].dist {
                            Self::estimate_old(*low_idx, closest_idx, old_centr, entries.as_slice(), indices.as_slice())
                        } else { 0 };
                    let dist_n = Self::estimate_new(centr0, centr1, high_idx, entries.as_slice(), indices.as_slice());
                    if dist_o + dist_n < old_dist {
                        self.clusters[*low_idx   ].centroid = old_centr;
                        self.clusters[closest_idx].centroid = centr0;
                        self.clusters[high_idx   ].centroid = centr1;
                        used[*low_idx]    = true;
                        used[closest_idx] = true;
                        used[high_idx]    = true;
                        high_u.remove(high_idx_idx);
                        do_elbg_step = true;
                    }
                }
            }
            iterations += 1;
        }
        if dist < prev_dist {
            for (cb, cluster) in old_cb.iter_mut().zip(self.clusters.iter()) {
                *cb = cluster.centroid;
            }
        }
        for i in 0..256 {
            dst[i][0] = old_cb[i].r;
            dst[i][1] = old_cb[i].g;
            dst[i][2] = old_cb[i].b;
        }
    }
}
