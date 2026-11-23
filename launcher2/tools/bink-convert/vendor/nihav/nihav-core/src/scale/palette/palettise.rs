pub fn find_nearest(pix: &[u8], pal: &[[u8; 3]; 256]) -> usize {
    let mut bestidx = 0;
    let mut bestdist = i32::MAX;

    for (idx, entry) in pal.iter().enumerate() {
        let dist0 = i32::from(pix[0]) - i32::from(entry[0]);
        let dist1 = i32::from(pix[1]) - i32::from(entry[1]);
        let dist2 = i32::from(pix[2]) - i32::from(entry[2]);
        if (dist0 | dist1 | dist2) == 0 {
            return idx;
        }
        let dist = dist0 * dist0 + dist1 * dist1 + dist2 * dist2;
        if bestdist > dist {
            bestdist = dist;
            bestidx  = idx;
        }
    }
    bestidx
}

pub struct LocalSearch {
    pal:        [[u8; 3]; 256],
    db:         Vec<Vec<[u8; 4]>>,
}

impl LocalSearch {
    fn quant(key: [u8; 3]) -> usize {
        (((key[0] >> 3) as usize) << 10) |
        (((key[1] >> 3) as usize) << 5) |
         ((key[2] >> 3) as usize)
    }
    pub fn new(in_pal: &[[u8; 3]; 256]) -> Self {
        let mut db = Vec::with_capacity(1 << 15);
        let pal = *in_pal;
        for _ in 0..(1 << 15) {
            db.push(Vec::new());
        }
        for (i, palentry) in pal.iter().enumerate() {
            let r0 = (palentry[0] >> 3) as usize;
            let g0 = (palentry[1] >> 3) as usize;
            let b0 = (palentry[2] >> 3) as usize;
            for r in r0.saturating_sub(1)..=(r0 + 1).min(31) {
                for g in g0.saturating_sub(1)..=(g0 + 1).min(31) {
                    for b in b0.saturating_sub(1)..=(b0 + 1).min(31) {
                        let idx = (r << 10) | (g << 5) | b;
                        db[idx].push([palentry[0], palentry[1], palentry[2], i as u8]);
                    }
                }
            }
        }
        Self { pal, db }
    }
    fn dist(a: &[u8; 4], b: [u8; 3]) -> u32 {
        let d0 = i32::from(a[0]) - i32::from(b[0]);
        let d1 = i32::from(a[1]) - i32::from(b[1]);
        let d2 = i32::from(a[2]) - i32::from(b[2]);
        (d0 * d0 + d1 * d1 + d2 * d2) as u32
    }
    pub fn search(&self, pix: [u8; 3]) -> usize {
        let idx = Self::quant(pix);
        let mut best_dist = u32::MAX;
        let mut best_idx = 0;
        let mut count = 0;
        for clr in self.db[idx].iter() {
            let dist = Self::dist(clr, pix);
            count += 1;
            if best_dist > dist {
                best_dist = dist;
                best_idx = clr[3] as usize;
                if dist == 0 { break; }
            }
        }
        if count > 0 {
            best_idx
        } else {
            find_nearest(&pix, &self.pal)
        }
    }
}

struct KDNode {
    key:    [u8; 3],
    comp:   u8,
    idx:    u8,
    child0: usize,
    child1: usize,
}

pub struct KDTree {
    nodes:  Vec<KDNode>,
}

fn avg_u8(a: u8, b: u8) -> u8 {
    (a & b) + ((a ^ b) >> 1)
}

impl KDTree {
    pub fn new(pal: &[[u8; 3]; 256]) -> Self {
        let mut npal = [[0; 4]; 256];
        for i in 0..256 {
            npal[i][0] = pal[i][0];
            npal[i][1] = pal[i][1];
            npal[i][2] = pal[i][2];
            npal[i][3] = i as u8;
        }
        let mut tree = Self { nodes: Vec::with_capacity(512) };
        tree.build(&mut npal, 0, 256, 1024, false);
        tree
    }
    fn build(&mut self, pal: &mut [[u8; 4]; 256], start: usize, end: usize, root: usize, child0: bool) {
        if start + 1 == end {
            let key = [pal[start][0], pal[start][1], pal[start][2]];
            let newnode = KDNode { key, comp: 0, idx: pal[start][3], child0: 0, child1: 0 };
            let cur_node = self.nodes.len();
            self.nodes.push(newnode);
            if child0 {
                self.nodes[root].child0 = cur_node;
            } else {
                self.nodes[root].child1 = cur_node;
            }
            return;
        }
        let mut min = [255u8; 3];
        let mut max = [0u8; 3];
        for clr in pal[start..end].iter() {
            for ((mi, ma), &c) in min.iter_mut().zip(max.iter_mut()).zip(clr.iter()) {
                *mi = (*mi).min(c);
                *ma = (*ma).max(c);
            }
        }
        let dr = max[0] - min[0];
        let dg = max[1] - min[1];
        let db = max[2] - min[2];
        let med = [avg_u8(min[0], max[0]), avg_u8(min[1], max[1]), avg_u8(min[2], max[2])];
        let comp = if dr > dg && dr > db {
                0
            } else if db > dr && db > dg {
                2
            } else {
                1
            };
        let pivot = Self::reorder(&mut pal[start..end], comp, med[comp]) + start;
        let newnode = KDNode { key: med, comp: comp as u8, idx: 0, child0: 0, child1: 0 };
        let cur_node = self.nodes.len();
        self.nodes.push(newnode);
        if root != 1024 {
            if child0 {
                self.nodes[root].child0 = cur_node;
            } else {
                self.nodes[root].child1 = cur_node;
            }
        }
        self.build(pal, start, pivot, cur_node, true);
        self.build(pal, pivot, end,   cur_node, false);
    }
    fn reorder(pal: &mut[[u8; 4]], comp: usize, med: u8) -> usize {
        let mut start = 0;
        let mut end = pal.len() - 1;
        while start < end {
            while start < end && pal[start][comp] <= med {
                start += 1;
            }
            while start < end && pal[end][comp] > med {
                end -= 1;
            }
            if start < end {
                pal.swap(start, end);
                start += 1;
                end   -= 1;
            }
        }
        start
    }
    pub fn search(&self, pix: [u8; 3]) -> usize {
        let mut idx = 0;
        loop {
            let cnode = &self.nodes[idx];
            if cnode.child0 == 0 {
                return cnode.idx as usize;
            }
            let nidx = if cnode.key[cnode.comp as usize] >= pix[cnode.comp as usize] { cnode.child0 } else { cnode.child1 };
            idx = nidx;
        }
    }
}
