use super::{VQElement, VQElementSum};

// very simple RNG for internal needs
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

struct Entry<T> {
    val:        T,
    count:      u64,
}

struct Cluster<T: VQElement, TS: VQElementSum<T>> {
    centroid:   T,
    dist:       u64,
    count:      u64,
    sum:        TS,
}

impl<T: VQElement, TS: VQElementSum<T>> Cluster<T, TS> {
    fn new(centroid: T) -> Self {
        Self {
            centroid,
            dist:       0,
            count:      0,
            sum:        TS::zero(),
        }
    }
    fn reset(&mut self) {
        self.count = 0;
        self.sum   = TS::zero();
        self.dist  = 0;
    }
    fn add_point(&mut self, entry: &Entry<T>) {
        self.sum.add(entry.val, entry.count);
        self.count += entry.count;
    }
    fn add_dist(&mut self, entry: &Entry<T>) {
        self.dist += u64::from(self.centroid.dist(entry.val)) * entry.count;
    }
    fn calc_centroid(&mut self) {
        if self.count > 0 {
            self.centroid = self.sum.get_centroid();
        }
    }
    fn calc_dist(&mut self) {
    }
}

pub struct ELBG<T: VQElement, TS: VQElementSum<T>> {
    clusters:   Vec<Cluster<T, TS>>,
}

impl<T: VQElement+Default, TS: VQElementSum<T>> ELBG<T, TS> {
    pub fn new(initial_cb: &[T]) -> Self {
        let mut clusters = Vec::with_capacity(initial_cb.len());
        for elem in initial_cb.iter() {
            let cluster = Cluster::new(*elem);
            clusters.push(cluster);
        }
        Self {
            clusters,
        }
    }
    fn new_split(old_index: usize, entries: &[Entry<T>], indices: &[usize]) -> Option<(T, T)> {
        let mut max = T::min_cw();
        let mut min = T::max_cw();
        let mut found = false;
        for (entry, idx) in entries.iter().zip(indices) {
            if *idx == old_index {
                max = max.max(entry.val);
                min = min.min(entry.val);
                found = true;
            }
        }
        if !found {
            return None;
        }
        let mut ts0 = TS::zero();
        let mut ts1 = TS::zero();
        ts0.add(min, 2); ts0.add(max, 1);
        ts1.add(min, 1); ts1.add(max, 2);
        Some((ts0.get_centroid(), ts1.get_centroid()))
    }
    fn old_centre(&self, old_index1: usize, old_index2: usize, entries: &[Entry<T>], indices: &[usize]) -> T {
        let mut max = T::min_cw();
        let mut min = T::max_cw();
        let mut found = false;
        for (entry, idx) in entries.iter().zip(indices) {
            if *idx == old_index1 || *idx == old_index2 {
                max = max.max(entry.val);
                min = min.min(entry.val);
                found = true;
            }
        }
        if !found {
            max = self.clusters[old_index1].centroid.max(self.clusters[old_index2].centroid);
            min = self.clusters[old_index1].centroid.min(self.clusters[old_index2].centroid);
        }
        let mut ts = TS::zero();
        ts.add(min, 2); ts.add(max, 1);
        ts.get_centroid()
    }
    fn estimate_old(old_idx0: usize, old_idx1: usize, c: T, entries: &[Entry<T>], indices: &[usize]) -> u64 {
        let mut clu: Cluster<T, TS> = Cluster::new(c);
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
    fn estimate_new(c0: T, c1: T, old_idx: usize, entries: &[Entry<T>], indices: &[usize]) -> u64 {
        let mut clu0: Cluster<T, TS> = Cluster::new(c0);
        let mut clu1: Cluster<T, TS> = Cluster::new(c1);
        let mut count0 = 0;
        let mut count1 = 0;
        for (entry, idx) in entries.iter().zip(indices) {
            if *idx == old_idx {
                if c0.dist(entry.val) < c1.dist(entry.val) {
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
    pub fn quantise(&mut self, src: &[T], dst: &mut [T]) -> usize {
        if src.is_empty() || dst.len() != self.clusters.len() {
            return 0;
        }
        let mut old_cb = vec![T::default(); self.clusters.len()];
        let mut prev_dist = u64::MAX;
        let mut dist = u64::MAX / 2;
        let mut indices = Vec::with_capacity(src.len());
        let mut elements = Vec::with_capacity(src.len());
        elements.extend_from_slice(src);
        for comp in 0..T::num_components() {
            T::sort_by_component(elements.as_mut_slice(), comp);
        }
        let mut entries = Vec::with_capacity(elements.len() / 2);
        let mut lastval = elements[0];
        let mut run = 1;
        for point in elements.iter().skip(1) {
            if &lastval == point {
                run += 1;
            } else {
                entries.push(Entry { val: lastval, count: run });
                lastval = *point;
                run = 1;
            }
        }
        entries.push(Entry { val: lastval, count: run });
        drop(elements);

        let mut cw_count = 0;
        let mut low_u:  Vec<usize> = Vec::with_capacity(self.clusters.len());
        let mut high_u: Vec<usize> = Vec::with_capacity(self.clusters.len());
        let mut rng = RNG::new();
        let mut iterations = 0usize;
        let mut do_elbg_step = true;
        while (iterations < 20) && (dist < prev_dist - prev_dist / 100) {
            prev_dist = dist;

            cw_count = 0;
            for cluster in self.clusters.iter() {
                if cluster.count == 0 {
                    continue;
                }
                old_cb[cw_count] = cluster.centroid;
                cw_count += 1;
            }
            for cluster in self.clusters.iter_mut() {
                cluster.reset();
            }

            // put points into the nearest clusters
            indices.clear();
            for entry in entries.iter() {
                let mut bestidx = 0;
                let mut bestdist = u32::MAX;
                for (i, cluster) in self.clusters.iter().enumerate() {
                    let dist = entry.val.dist(cluster.centroid);
                    if bestdist > dist {
                        bestdist = dist;
                        bestidx = i;
                        if dist == 0 {
                            break;
                        }
                    }
                }
                indices.push(bestidx);
                self.clusters[bestidx].add_point(entry);
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

            let dmean = dist / (dst.len() as u64);
            low_u.clear();
            high_u.clear();
            let mut used = vec![false; dst.len()];
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
                    for (i, (&used, cluster)) in used.iter().zip(self.clusters.iter()).take(dst.len()).enumerate() {
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
                    if let Some((centr0, centr1)) = ret {
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
            }
            iterations += 1;
        }
        if dist < prev_dist {
            cw_count = 0;
            for cluster in self.clusters.iter() {
                if cluster.count == 0 {
                    continue;
                }
                old_cb[cw_count] = cluster.centroid;
                cw_count += 1;
            }
        }
        dst.copy_from_slice(&old_cb);
        cw_count
    }
}
