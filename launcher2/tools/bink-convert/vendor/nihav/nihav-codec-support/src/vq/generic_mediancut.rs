use super::{VQElement, VQElementSum};

struct VQBox<'a, T: VQElement> {
    points: &'a mut [T],
    max:    T,
    min:    T,
}

impl<'a, T: VQElement> VQBox<'a, T> {
    fn new(points: &'a mut [T]) -> Self {
        let mut max = T::min_cw();
        let mut min = T::max_cw();
        for point in points.iter() {
            max = max.max(*point);
            min = min.min(*point);
        }
        Self { points, max, min }
    }
    fn can_split(&self) -> bool {
        self.max != self.min
    }
    fn calc_min_and_max(points: &[T]) -> (T, T) {
        let mut max = T::min_cw();
        let mut min = T::max_cw();
        for point in points.iter() {
            max = max.max(*point);
            min = min.min(*point);
        }
        (min, max)
    }
    fn get_pivot(arr: &[T]) -> usize {
        if arr.len() < 2 {
            return 0;
        }
        let mut lastval = arr[0];
        let mut pivot = 0;
        let mut idx = 1;
        for el in arr.iter().skip(1) {
            if *el != lastval && (pivot == 0 || idx <= arr.len() / 2) {
                pivot = idx;
                lastval = *el;
            }
            idx += 1;
        }
        pivot
    }
    fn split(self) -> (VQBox<'a, T>, VQBox<'a, T>) {
        let sort_c = T::max_dist_component(&self.min, &self.max);
        T::sort_by_component(self.points, sort_c);
        let pivot = Self::get_pivot(self.points);
        let (part0, part1) = self.points.split_at_mut(pivot);
        let (min0, max0) = Self::calc_min_and_max(part0);
        let (min1, max1) = Self::calc_min_and_max(part1);
        let box0 = VQBox { points: part0, max: max0, min: min0 };
        let box1 = VQBox { points: part1, max: max1, min: min1 };

        (box0, box1)
    }
}

pub fn quantise_median_cut<T: VQElement, TS: VQElementSum<T>>(src: &[T], dst: &mut [T]) -> usize {
    let mut points = Vec::with_capacity(src.len());
    points.extend(src.iter());
    for comp in 0..T::num_components() {
        T::sort_by_component(points.as_mut_slice(), comp);
    }
    let box0 = VQBox::new(points.as_mut_slice());
    let mut boxes: Vec<VQBox<T>> = Vec::with_capacity(dst.len());
    boxes.push(box0);
    let mut changed = true;
    while changed && boxes.len() < dst.len() {
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
            if boxes.len() == dst.len() {
                break;
            }
        }
        if split_largest {
            let mut maxidx = 0;
            let mut lcount = 0;
            for (i, cbox) in boxes.iter().enumerate() {
                if cbox.can_split() && cbox.points.len() > lcount {
                    lcount = cbox.points.len();
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
    for (dst, curbox) in dst.iter_mut().zip(boxes.iter()) {
        let mut sum = TS::zero();
        sum.add(curbox.min, 1);
        sum.add(curbox.max, 1);
        *dst = sum.get_centroid();
    }

    boxes.len()
}
