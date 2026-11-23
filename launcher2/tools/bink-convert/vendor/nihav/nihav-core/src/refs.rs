//! Reference-counted buffer data type.
//!
//! NihAV requires some reference-counted type especially for frame buffer pools.
//! `Arc` does not allow mutability with several references present, `RwLock` does not work reliably in a single thread mode (at least for me) so I ended up NIHing something.
//!
//! Currently it does not prevent code from reading the data that is being written to.
//! Maybe in the future this will be replaced by something better and using more standard components.
//!
//! Also it contains `unsafe{}` so I should not write in Rust at all.
//!
//! # Examples
//!
//! ```
//! use nihav_core::refs::NABufferRef;
//!
//! let vec = vec![42u8; 16];
//! let vec_ref = NABufferRef::new(vec);
//! let vec_ref2 = vec_ref.clone();
//! let ref_count = vec_ref.get_num_refs(); // should be 2
//! println!("vector element 4 is {}", vec_ref[4]); // should print the fourth vector element
//! ```
use std::ops::{Deref, DerefMut};
use std::convert::AsRef;
use std::sync::atomic::*;

struct NABufferData<T> {
    data:       T,
    refs:       AtomicUsize,
}

impl<T> NABufferData<T> {
    fn new(data: T) -> Self {
        Self {
            data,
            refs:       AtomicUsize::new(1),
        }
    }
    fn inc_refs(obj: &mut Self) {
        obj.refs.fetch_add(1, Ordering::SeqCst);
    }
    fn dec_refs(obj: &mut Self) -> bool {
        obj.refs.fetch_sub(1, Ordering::SeqCst) == 1
    }
    fn get_num_refs(obj: &Self) -> usize {
        obj.refs.load(Ordering::Relaxed)
    }
    fn get_read_ptr(obj: &Self) -> &T {
        &obj.data
    }
    fn get_write_ptr(obj: &mut Self) -> Option<&mut T> {
        Some(&mut obj.data)
    }
}

/// Reference-counted buffer reference.
pub struct NABufferRef<T> {
    ptr: *mut NABufferData<T>,
}

unsafe impl<T> Sync for NABufferRef<T> {}
unsafe impl<T> Send for NABufferRef<T> {}

impl<T> NABufferRef<T> {
    /// Constructs a new instance of `NABufferRef`.
    pub fn new(val: T) -> Self {
        let bdata = NABufferData::new(val);
        let nbox: Box<_> = Box::new(bdata);
        Self { ptr: Box::into_raw(nbox) }
    }
    /// Reports the number of references for the current instance.
    pub fn get_num_refs(&self) -> usize {
        unsafe {
            NABufferData::get_num_refs(self.ptr.as_mut().unwrap())
        }
    }
    /// Returns a mutable pointer to the underlying data if possible.
    pub fn as_mut(&mut self) -> Option<&mut T> {
        unsafe {
            NABufferData::get_write_ptr(self.ptr.as_mut().unwrap())
        }
    }
}

impl<T> AsRef<T> for NABufferRef<T> {
    fn as_ref(&self) -> &T {
        unsafe {
            NABufferData::get_read_ptr(self.ptr.as_mut().unwrap())
        }
    }
}

impl<T> Deref for NABufferRef<T> {
    type Target = T;
    fn deref(&self) -> &T { self.as_ref() }
}

impl<T> DerefMut for NABufferRef<T> {
    fn deref_mut(&mut self) -> &mut T { self.as_mut().unwrap() }
}

impl<T> Clone for NABufferRef<T> {
    fn clone(&self) -> Self {
        unsafe {
            NABufferData::inc_refs(self.ptr.as_mut().unwrap());
        }
        Self { ptr: self.ptr }
    }
}

impl<T> Drop for NABufferRef<T> {
    fn drop(&mut self) {
        unsafe {
            if NABufferData::dec_refs(self.ptr.as_mut().unwrap()) {
                let data = Box::from_raw(self.ptr);
                std::mem::drop(data);
            }
        }
    }
}

impl<T:Default> Default for NABufferRef<T> {
    fn default() -> Self {
        Self::new(T::default())
    }
}
