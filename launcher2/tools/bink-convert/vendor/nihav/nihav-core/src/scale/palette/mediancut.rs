use super::Pixel;

struct VQBox<'a> {
    pixels: &'a mut [Pixel],
    max:    Pixel,
    min:    Pixel,
}

fn avg_u8(a: u8, b: u8) -> u8 {
    (a & b) + ((a ^ b) >> 1)
}

impl<'a> VQBox<'a> {
    fn new(pixels: &'a mut [Pixel]) -> Self {
        let mut max = Pixel{ r:   0, g:   0, b:   0 };
        let mut min = Pixel{ r: 255, g: 255, b: 255 };
        for pix in pixels.iter() {
            max = max.max(*pix);
            min = min.min(*pix);
        }
        Self { pixels, max, min }
    }
    fn can_split(&self) -> bool {
        let dr = self.max.r - self.min.r;
        let dg = self.max.g - self.min.g;
        let db = self.max.b - self.min.b;
        (dr | dg | db) != 0
    }
    fn sort<F>(arr: &mut [Pixel], idx: F) -> usize where F: Fn(&Pixel) -> usize {
        let mut buckets: Vec<Vec<Pixel>> = Vec::with_capacity(256);
        let mut counts = [0; 256];
        for pix in arr.iter() {
            counts[idx(pix)] += 1;
        }
        for &count in counts.iter() {
            buckets.push(Vec::with_capacity(count));
        }
        for pix in arr.iter() {
            buckets[idx(pix)].push(*pix);
        }
        let mut start = 0;
        let mut pivot = 0;
        let mut cand1 = 0;
        for bucket in buckets.iter() {
            let end = bucket.len();
            if (start > 0) && (start <= arr.len() / 2) {
                pivot = start + end;
                cand1 = start;
            }
            arr[start..][..end].copy_from_slice(bucket.as_slice());
            start += end;
        }
        for bucket in buckets.iter() {
            if pivot != 0 {
                break;
            }
            pivot += bucket.len();
        }
        if pivot < arr.len() || cand1 == 0 {
            pivot
        } else {
            cand1
        }
    }
    fn calc_min_and_max(pixels: &[Pixel]) -> (Pixel, Pixel) {
        let mut max = Pixel{ r:   0, g:   0, b:   0 };
        let mut min = Pixel{ r: 255, g: 255, b: 255 };
        for pix in pixels.iter() {
            max = max.max(*pix);
            min = min.min(*pix);
        }
        (min, max)
    }
    fn split(self) -> (VQBox<'a>, VQBox<'a>) {
        let dr = self.max.r - self.min.r;
        let dg = self.max.g - self.min.g;
        let db = self.max.b - self.min.b;

        let box0;
        let box1;
        if (dr > dg) && (dr >= db) {
            let pivot = Self::sort(self.pixels, |pix| pix.r as usize);
            let (part0, part1) = self.pixels.split_at_mut(pivot);
            let (min0, max0) = Self::calc_min_and_max(part0);
            let (min1, max1) = Self::calc_min_and_max(part1);
            box0 = VQBox { pixels: part0, max: max0, min: min0 };
            box1 = VQBox { pixels: part1, max: max1, min: min1 };
        } else if (db > dr) && (db > dg) {
            let pivot = Self::sort(self.pixels, |pix| pix.b as usize);
            let (part0, part1) = self.pixels.split_at_mut(pivot);
            let (min0, max0) = Self::calc_min_and_max(part0);
            let (min1, max1) = Self::calc_min_and_max(part1);
            box0 = VQBox { pixels: part0, max: max0, min: min0 };
            box1 = VQBox { pixels: part1, max: max1, min: min1 };
        } else {
            let pivot = Self::sort(self.pixels, |pix| pix.g as usize);
            let (part0, part1) = self.pixels.split_at_mut(pivot);
            let (min0, max0) = Self::calc_min_and_max(part0);
            let (min1, max1) = Self::calc_min_and_max(part1);
            box0 = VQBox { pixels: part0, max: max0, min: min0 };
            box1 = VQBox { pixels: part1, max: max1, min: min1 };
        }
        (box0, box1)
    }
}

pub fn quantise_median_cut(src: &[Pixel], pal: &mut [[u8; 3]; 256]) -> usize {
    let mut pixels = Vec::with_capacity(src.len());
    pixels.extend_from_slice(src);
    VQBox::sort(pixels.as_mut_slice(), |pix| pix.r as usize);
    VQBox::sort(pixels.as_mut_slice(), |pix| pix.g as usize);
    VQBox::sort(pixels.as_mut_slice(), |pix| pix.b as usize);
    let box0 = VQBox::new(pixels.as_mut_slice());
    let mut boxes: Vec<VQBox> = Vec::with_capacity(256);
    boxes.push(box0);
    let mut changed = true;
    while changed && boxes.len() < 256 {
        let end = boxes.len();
        changed = false;
        let mut split_largest = false;
        for _ in 0..end {
            let curbox = boxes.remove(0);
            if curbox.can_split() {
                let (box0, box1) = curbox.split();
                boxes.push(box0);
                boxes.push(box1);
                changed = true;
            } else {
                boxes.push(curbox);
                split_largest = true;
                break;
            }
            if boxes.len() == 256 {
                break;
            }
        }
        if split_largest {
            let mut maxidx = 0;
            let mut lcount = 0;
            for (i, cbox) in boxes.iter().enumerate() {
                if cbox.can_split() && cbox.pixels.len() > lcount {
                    lcount = cbox.pixels.len();
                    maxidx = i;
                }
            }
            if lcount > 0 {
                let curbox = boxes.remove(maxidx);
                let (box0, box1) = curbox.split();
                boxes.push(box0);
                boxes.push(box1);
                changed = true;
            }
        }
    }
    for (curbox, palentry) in boxes.iter().zip(pal.iter_mut()) {
        palentry[0] = avg_u8(curbox.min.r, curbox.max.r);
        palentry[1] = avg_u8(curbox.min.g, curbox.max.g);
        palentry[2] = avg_u8(curbox.min.b, curbox.max.b);
    }

    boxes.len()
}
