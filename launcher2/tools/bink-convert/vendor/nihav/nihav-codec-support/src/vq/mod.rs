//! Vector quantisation routines.
mod generic_elbg;
mod generic_mediancut;

pub trait VQElement: Sized+Copy+PartialEq {
    fn dist(&self, rval: Self) -> u32;
    fn min_cw() -> Self;
    fn max_cw() -> Self;
    fn min(&self, rval: Self) -> Self;
    fn max(&self, rval: Self) -> Self;
    fn num_components() -> usize;
    fn sort_by_component(arr: &mut [Self], component: usize);
    fn max_dist_component(min: &Self, max: &Self) -> usize;
}

pub trait VQElementSum<T: VQElement> {
    fn zero() -> Self;
    fn add(&mut self, rval: T, count: u64);
    fn get_centroid(&self) -> T;
}

pub use self::generic_elbg::ELBG;
pub use self::generic_mediancut::quantise_median_cut;
