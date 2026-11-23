// Copyright (C) 2026 WWE 2K14 Native contributors.
// BIKi writer adapted from the project's previously private bk2enc tool and released here under
// AGPL-3.0-only. Scan and quantiser tables are derived from NihAV; see NOTICE.md.
// Bink 1 revision i uses skip and intra blocks; the game accepts revision i.
use crate::tables::*;

struct Bw { buf: Vec<u8>, acc: u64, n: u32 }
impl Bw {
    fn new() -> Self { Bw { buf: Vec::new(), acc: 0, n: 0 } }
    fn put(&mut self, v: u32, bits: u32) {
        if bits == 0 { return; }
        self.acc |= ((v as u64) & ((1u64 << bits) - 1)) << self.n;
        self.n += bits;
        while self.n >= 8 { self.buf.push(self.acc as u8); self.acc >>= 8; self.n -= 8; }
    }
    fn flag(&mut self, b: bool) { self.put(b as u32, 1); }
    fn tell(&self) -> usize { self.buf.len() * 8 + self.n as usize }
    fn align32(&mut self) { while self.tell() & 31 != 0 { self.put(0, 1); } }
}
fn calc_len(size: usize) -> u32 { 32 - ((size + 511) as u32).leading_zeros() }
fn bitlen(v: u32) -> u32 { 32 - v.leading_zeros() }

const A1: i32 = 2896; const A2: i32 = 2217; const A3: i32 = 3784; const A4: i32 = -5352;
fn idct1(s: &[i32], ss: usize, d: &mut [i32], ds: usize, o: usize, bias: i32, shift: u32) {
    let g = |i: usize| s[o + i * ss];
    let a0 = g(0) + g(4); let a1 = g(0) - g(4); let a2 = g(2) + g(6);
    let a3 = A1.wrapping_mul(g(2) - g(6)) >> 11;
    let a4 = g(5) + g(3); let a5 = g(5) - g(3); let a6 = g(1) + g(7); let a7 = g(1) - g(7);
    let b0 = a4 + a6; let b1 = A3.wrapping_mul(a5 + a7) >> 11;
    let b2 = (A4.wrapping_mul(a5) >> 11) - b0 + b1;
    let b3 = (A1.wrapping_mul(a6 - a4) >> 11) - b2;
    let b4 = (A2.wrapping_mul(a7) >> 11) + b3 - b1;
    let c0 = a0 + a2; let c1 = a0 - a2; let c2 = a1 + (a3 - a2); let c3 = a1 - (a3 - a2);
    d[o + 0 * ds] = (c0 + b0 + bias) >> shift; d[o + 1 * ds] = (c2 + b2 + bias) >> shift;
    d[o + 2 * ds] = (c3 + b3 + bias) >> shift; d[o + 3 * ds] = (c1 - b4 + bias) >> shift;
    d[o + 4 * ds] = (c1 + b4 + bias) >> shift; d[o + 5 * ds] = (c3 - b3 + bias) >> shift;
    d[o + 6 * ds] = (c2 - b2 + bias) >> shift; d[o + 7 * ds] = (c0 - b0 + bias) >> shift;
}
fn idct(co: &[i32; 64], out: &mut [i32; 64]) {
    let mut t = [0i32; 64];
    for i in 0..8 { idct1(co, 8, &mut t, 8, i, 0, 0); }
    for r in 0..8 {
        let mut row = [0i32; 8];
        idct1(&t[r * 8..r * 8 + 8], 1, &mut row, 1, 0, 0x7F, 8);
        out[r * 8..r * 8 + 8].copy_from_slice(&row);
    }
}

struct Basis { b: Vec<[f64; 64]>, norm: Vec<f64> }
fn make_basis() -> Basis {
    let mut b = Vec::new();
    let mut norm = Vec::new();
    for pos in 0..64 {
        let mut co = [0i32; 64];
        co[pos] = 1 << 16;
        let mut o = [0i32; 64];
        idct(&co, &mut o);
        let mut f = [0f64; 64];
        let mut n = 0.0;
        for i in 0..64 { f[i] = o[i] as f64 / (1u32 << 16) as f64; n += f[i] * f[i]; }
        b.push(f);
        norm.push(n);
    }
    Basis { b, norm }
}

// Mirrors the decoder's coefficient reader: levels are in scan order, index 1..63.
fn write_coefs(bw: &mut Bw, lv: &[i32; 64]) {
    let blen = |i: usize| bitlen(lv[i].unsigned_abs());
    let mut top = 0;
    for i in 1..64 { top = top.max(blen(i)); }
    assert!(top <= 15);
    bw.put(top, 4);
    let mut clist = [0i32; 128];
    let mut mlist = [0u8; 128];
    let (mut ls, mut le) = (64usize, 64usize);
    for &(c, m) in &[(4, 0u8), (24, 0), (44, 0), (1, 3), (2, 3), (3, 3)] { clist[le] = c; mlist[le] = m; le += 1; }
    let mut p = top;
    let coef = |bw: &mut Bw, idx: usize, p: u32| {
        let v = lv[idx];
        let a = v.unsigned_abs();
        if p > 1 { bw.put(a & ((1 << (p - 1)) - 1), p - 1); }
        bw.flag(v < 0);
    };
    while p >= 1 {
        let mut lp = ls;
        while lp < le {
            let c = clist[lp];
            let m = mlist[lp];
            if m == 0 && c == 0 { lp += 1; continue; }
            let span = match m { 0 => 20usize, 1 => 16, 2 => 4, _ => 1 };
            let any = (0..span).any(|i| blen(c as usize + i) == p);
            bw.flag(any);
            if !any { lp += 1; continue; }
            match m {
                0 | 2 => {
                    if m == 0 { clist[lp] = c + 4; mlist[lp] = 1; }
                    else { clist[lp] = 0; mlist[lp] = 0; lp += 1; }
                    for i in 0..4 {
                        let idx = c as usize + i;
                        if blen(idx) == p { bw.flag(false); coef(bw, idx, p); }
                        else { bw.flag(true); ls -= 1; clist[ls] = idx as i32; mlist[ls] = 3; }
                    }
                }
                1 => {
                    mlist[lp] = 2;
                    for i in 0..3 { clist[le] = c + i * 4 + 4; mlist[le] = 2; le += 1; }
                }
                _ => { coef(bw, c as usize, p); clist[lp] = 0; mlist[lp] = 0; lp += 1; }
            }
        }
        p -= 1;
    }
}
struct Enc { q: usize, skip10: u32, basis: Basis }
struct BlockOut { lv: [i32; 64], q: usize, dc: u32 }

pub struct YuvFrame { pub y: Vec<u8>, pub u: Vec<u8>, pub v: Vec<u8> }

struct PlaneRow { types: Vec<u32>, dcs: Vec<u32>, blocks: Vec<BlockOut>, rec: Vec<u8>, all_intra: bool }

fn encode_block(e: &Enc, src: &[i32; 64], rec: &mut [u8; 64]) -> BlockOut {
    let mut s = *src;
    loop {
        let mut pr = [0f64; 64];
        for pos in 0..64 {
            let mut d = 0.0;
            for i in 0..64 { d += e.basis.b[pos][i] * s[i] as f64; }
            pr[pos] = d / e.basis.norm[pos];
        }
        let mut q = e.q as isize;
        while q >= 0 {
            let qm = &BINK_INTRA_QUANT[q as usize];
            let mut lv = [0i32; 64];
            let mut co = [0i32; 64];
            let dc = (pr[0] * 2048.0 / qm[0] as f64).round().max(0.0).min(2047.0) as i32;
            co[0] = dc.wrapping_mul(qm[0]) >> 11;
            for idx in 1..64 {
                let pos = BINK_SCAN[idx];
                let l = (pr[pos] * 2048.0 / qm[idx] as f64).round() as i32;
                lv[idx] = l;
                co[pos] = l.wrapping_mul(qm[idx]) >> 11;
            }
            let mut o = [0i32; 64];
            idct(&co, &mut o);
            // The decoder does not clamp, so an overshoot would wrap around: use a finer quantiser instead.
            if o.iter().all(|&v| v >= 0 && v <= 255) {
                for i in 0..64 { rec[i] = o[i] as u8; }
                return BlockOut { lv, q: q as usize, dc: dc as u32 };
            }
            q -= 1;
        }
        let m = s.iter().sum::<i32>() / 64;
        for v in s.iter_mut() { *v = m + (*v - m) * 15 / 16; }
    }
}

fn write_types(bw: &mut Bw, v: &[u32], bits: u32) {
    bw.put(v.len() as u32, bits);
    if v.iter().all(|&x| x == v[0]) { bw.flag(true); bw.put(v[0], 4); return; }
    bw.flag(false);
    let mut last = 0u32;
    let mut i = 0;
    while i < v.len() {
        if v[i] == last {
            let mut run = 0;
            while i + run < v.len() && v[i + run] == last { run += 1; }
            if run >= 32 { bw.put(15, 4); i += 32; continue; }
            if run >= 12 { bw.put(14, 4); i += 12; continue; }
            if run >= 8 { bw.put(13, 4); i += 8; continue; }
            if run >= 4 { bw.put(12, 4); i += 4; continue; }
        }
        bw.put(v[i], 4);
        last = v[i];
        i += 1;
    }
}
fn write_dcs(bw: &mut Bw, v: &[u32], bits: u32) {
    bw.put(v.len() as u32, bits);
    let mut val = v[0] as i32;
    bw.put(val as u32, 11);
    let mut i = 1;
    while i < v.len() {
        let n = (v.len() - i).min(8);
        let mut t = val;
        let mut mx = 0u32;
        for k in 0..n { let d = v[i + k] as i32 - t; mx = mx.max(d.unsigned_abs()); t = v[i + k] as i32; }
        let b = bitlen(mx);
        bw.put(b, 4);
        if b != 0 {
            for k in 0..n {
                let d = v[i + k] as i32 - val;
                bw.put(d.unsigned_abs(), b);
                if d != 0 { bw.flag(d < 0); }
                val = v[i + k] as i32;
            }
        }
        i += n;
    }
}

fn encode_row(e: &Enc, src: &[u8], last: Option<&[u8]>, w: usize, by: usize) -> PlaneRow {
    let bwd = w / 8;
    let mut types = Vec::with_capacity(bwd);
    let mut dcs = Vec::new();
    let mut blocks = Vec::new();
    let mut rec = vec![0u8; w * 8];
    let mut all_intra = true;
    for bx in 0..bwd {
        let mut s = [0i32; 64];
        let (mut sad, mut mx) = (0u32, 0u32);
        for y in 0..8 {
            for x in 0..8 {
                let o = (by * 8 + y) * w + bx * 8 + x;
                s[y * 8 + x] = src[o] as i32;
                if let Some(l) = last {
                    let d = (src[o] as i32 - l[o] as i32).unsigned_abs();
                    sad += d;
                    mx = mx.max(d);
                }
            }
        }
        if last.is_some() && sad * 10 <= e.skip10 * 64 && mx <= 8 {
            all_intra = false;
            types.push(0);
            let l = last.unwrap();
            for y in 0..8 {
                let o = y * w + bx * 8;
                rec[o..o + 8].copy_from_slice(&l[(by * 8 + y) * w + bx * 8..(by * 8 + y) * w + bx * 8 + 8]);
            }
        } else {
            let mut r = [0u8; 64];
            let b = encode_block(e, &s, &mut r);
            for y in 0..8 {
                let o = y * w + bx * 8;
                rec[o..o + 8].copy_from_slice(&r[y * 8..y * 8 + 8]);
            }
            types.push(5);
            dcs.push(b.dc);
            blocks.push(b);
        }
    }
    PlaneRow { types, dcs, blocks, rec, all_intra }
}

// Returns true when no block in the plane was skipped. Rows encode independently, so release
// builds can divide this work among threads without changing bitstream order or decisions.
fn encode_plane(e: &Enc, bw: &mut Bw, src: &[u8], last: Option<&[u8]>, rec: &mut [u8], w: usize, h: usize, threads: usize) -> bool {
    let (bwd, bh) = (w / 8, h / 8);
    let lb_t = calc_len(w >> 3);
    let lb_s = calc_len(w >> 4);
    let lb_c = calc_len(bwd * 64);
    let lb_p = calc_len(bwd * 8);
    let lb_x = calc_len(w >> 3);
    let lb_dc = calc_len(w >> 3);
    let lb_r = calc_len(bwd * 48);
    let rows = if threads <= 1 || bh <= 1 {
        (0..bh).map(|by| encode_row(e, src, last, w, by)).collect::<Vec<_>>()
    } else {
        let workers = threads.min(bh);
        std::thread::scope(|scope| {
            let mut jobs = Vec::with_capacity(workers);
            for worker in 0..workers {
                let start = bh * worker / workers;
                let end = bh * (worker + 1) / workers;
                jobs.push(scope.spawn(move || {
                    (start..end).map(|by| encode_row(e, src, last, w, by)).collect::<Vec<_>>()
                }));
            }
            let mut rows = Vec::with_capacity(bh);
            for job in jobs { rows.extend(job.join().expect("Bink row worker failed")); }
            rows
        })
    };
    // Descriptors, flat tree 0 (4 bits per symbol): block types, sub types, 16 colour-high, colours, patterns, x, y, runs.
    for _ in 0..(1 + 1 + 16 + 1 + 1 + 1 + 1 + 1) { bw.put(0, 4); }
    // A length is read only when nothing is pending; zero ends a bundle for this plane, so a row with no
    // DC values takes the next non-empty row's values early.
    let mut dc_left = 0usize;
    let mut dc_ended = false;
    let mut all_intra = true;
    for by in 0..bh {
        let row = &rows[by];
        all_intra &= row.all_intra;
        rec[by * 8 * w..(by + 1) * 8 * w].copy_from_slice(&row.rec);
        write_types(bw, &row.types, lb_t);
        if by == 0 {
            bw.put(0, lb_s); bw.put(0, lb_c); bw.put(0, lb_p); bw.put(0, lb_x); bw.put(0, lb_x);
        }
        if dc_left == 0 && !dc_ended {
            match (by..bh).find(|&k| !rows[k].dcs.is_empty()) {
                Some(k) => { write_dcs(bw, &rows[k].dcs, lb_dc); dc_left = rows[k].dcs.len(); }
                None => { bw.put(0, lb_dc); dc_ended = true; }
            }
        }
        if by == 0 { bw.put(0, lb_dc); bw.put(0, lb_r); }
        dc_left -= row.dcs.len();
        for b in &row.blocks {
            write_coefs(bw, &b.lv);
            bw.put(b.q as u32, 4);
        }
    }
    bw.align32();
    all_intra
}

pub struct BinkIEncoder {
    width: usize,
    height: usize,
    encoder: Enc,
    last_y: Vec<u8>,
    last_u: Vec<u8>,
    last_v: Vec<u8>,
    frames: Vec<(Vec<u8>, bool)>,
    sq: f64,
    count: f64,
}

impl BinkIEncoder {
    pub fn new(width: usize, height: usize) -> Self {
        BinkIEncoder {
            width, height, encoder: Enc { q: 13, skip10: 25, basis: make_basis() },
            last_y: vec![0; width * height], last_u: vec![0; width * height / 4],
            last_v: vec![0; width * height / 4], frames: Vec::new(), sq: 0.0, count: 0.0,
        }
    }

    pub fn push(&mut self, frame: YuvFrame, threads: usize) -> Result<(), String> {
        let w = self.width;
        let h = self.height;
        let cw = w / 2;
        let ch = h / 2;
        if frame.y.len() != w * h || frame.u.len() != cw * ch || frame.v.len() != cw * ch {
            return Err("The decoder returned an incomplete YUV 4:2:0 frame.".into());
        }
        let (mut ry, mut ru, mut rv) = (vec![0u8; w * h], vec![0u8; cw * ch], vec![0u8; cw * ch]);
        let mut bw = Bw::new();
        bw.put(0, 32); // revision i: offset from here to the chroma planes, as in the game's own BIKi files
        let first = self.frames.is_empty();
        let mut intra = encode_plane(&self.encoder, &mut bw, &frame.y,
            if first { None } else { Some(&self.last_y) }, &mut ry, w, h, threads);
        let chroma_at = bw.buf.len() as u32;
        // Revision h and later store V before U.
        intra &= encode_plane(&self.encoder, &mut bw, &frame.v,
            if first { None } else { Some(&self.last_v) }, &mut rv, cw, ch, threads);
        intra &= encode_plane(&self.encoder, &mut bw, &frame.u,
            if first { None } else { Some(&self.last_u) }, &mut ru, cw, ch, threads);
        let mut pkt = bw.buf;
        pkt[..4].copy_from_slice(&chroma_at.to_le_bytes());
        if pkt.len() % 4 != 0 { return Err("The encoder produced a misaligned BIKi frame.".into()); }
        for (x, y) in frame.y.iter().zip(ry.iter()) { let d = *x as f64 - *y as f64; self.sq += d * d; self.count += 1.0; }
        self.last_y = ry;
        self.last_u = ru;
        self.last_v = rv;
        self.frames.push((pkt, intra));
        Ok(())
    }

    pub fn finish(self, tb_den: u32, tb_num: u32) -> Result<(Vec<u8>, usize, f64), String> {
        let nf = self.frames.len();
        if nf == 0 { return Err("The input contains no decodable video frames.".into()); }
        let table = 44usize.checked_add(4usize.checked_mul(nf + 1).ok_or("The frame table is too large.")?)
            .ok_or("The frame table is too large.")?;
        let payload: usize = self.frames.iter().try_fold(0usize, |sum, f| sum.checked_add(f.0.len()))
            .ok_or("The encoded movie is too large.")?;
        let total = table.checked_add(payload).ok_or("The encoded movie is too large.")?;
        let largest = self.frames.iter().map(|f| f.0.len()).max().unwrap_or(0);
        if total > u32::MAX as usize || nf > u32::MAX as usize || largest > u32::MAX as usize ||
           self.width > u32::MAX as usize || self.height > u32::MAX as usize {
            return Err("The encoded movie exceeds the BIKi size limit.".into());
        }
        let mut out = Vec::with_capacity(total);
        out.extend_from_slice(b"BIKi");
        for v in &[(total - 8) as u32, nf as u32, largest as u32, nf as u32, self.width as u32, self.height as u32,
                   tb_den, tb_num, 0u32, 0u32] {
            out.extend_from_slice(&v.to_le_bytes());
        }
        let mut off = table;
        for (p, keyframe) in &self.frames {
            out.extend_from_slice(&((off as u32) | (*keyframe as u32)).to_le_bytes());
            off += p.len();
        }
        out.extend_from_slice(&(off as u32).to_le_bytes());
        for (packet, _) in &self.frames { out.extend_from_slice(packet); }
        let psnr = if self.sq == 0.0 { f64::INFINITY } else { 10.0 * (255.0 * 255.0 / (self.sq / self.count)).log10() };
        Ok((out, largest, psnr))
    }
}
