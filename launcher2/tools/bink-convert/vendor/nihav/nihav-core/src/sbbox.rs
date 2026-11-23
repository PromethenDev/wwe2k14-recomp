//! Self-borrowing structure.
//!
//! Occasionally it is necessary (or merely more convenient) to track both a certain object and some exclusive borrower of it e.g. file handle and a writer borrowing it for the duration of its lifetime.
//! While it is better to have a different design that e.g. incorporates borrowed object and returns it at the end or merely borrows it at each call, sometimes it is just easier to have a wrapper object that holds both borrowed and borrower object and grants exclusive access to one of them.
//!
//! `SelfBorrow` offers exactly such functionality. Since it has to rely on certain unsafe language features, users should use it with caution (and maybe consider `borrow` crate instead).

use std::pin::Pin;
use std::alloc::{dealloc, Layout};

/// Self-borrowing structure that contains borrowed object of type `T` and its borrower of type `U`.
///
/// It should be created with e.g. [`try_new`] and then use [`get_object_mut`] to access the borrower object.
/// When this object is no longer required, [`end`] may be called to get the original borrowed object back.
///
/// # Example:
///
/// ```
/// use nihav_core::sbbox::*;
///
/// struct PlusOne<'a> { v: &'a mut u8 }
/// impl<'a> PlusOne<'a> {
///     fn invoke(&mut self) { *self.v += 1; }
/// }
///
/// let val = 42u8;
/// let mut sbbox = SelfBorrow::new(val, |v| PlusOne{ v: unsafe { v.as_mut().unwrap() } });
/// sbbox.get_object_mut().invoke();
/// let orig_val = SelfBorrow::end(sbbox);
/// println!("new value is {orig_val}");
/// ```
///
/// [`try_new`]: ./struct.SelfBorrow.html#method.try_new
/// [`get_object_mut`]: ./struct.SelfBorrow.html#method.get_object_mut
/// [`end`]: ./struct.SelfBorrow.html#method.end
pub struct SelfBorrow<T, U> {
    bval: T,
    dst:  Option<U>,
}

impl<T: Unpin, U: Unpin> SelfBorrow<T, U> {
    /// Creates a new self-borrow box from a borrowed object and a function taking a pointer to it.
    pub fn new<F>(src: T, create: F) -> Pin<Box<Self>>
            where F: Fn(*mut T) -> U {
        let mut obj = Box::pin(Self{
            bval: src,
            dst: None,
        });
        let ptr = &mut obj.bval as *mut T;
        obj.dst = Some(create(ptr));
        obj
    }
    /// Attempts to create a new self-borrow box from a borrowed object and a function taking a pointer to it.
    ///
    /// In case `create` function fails, the original object will be returned instead.
    pub fn try_new<F>(src: T, create: F) -> Result<Pin<Box<Self>>, T>
            where F: Fn(*mut T) -> Option<U> {
        let mut obj = Box::pin(Self{
            bval: src,
            dst: None,
        });
        let ptr = &mut obj.bval as *mut T;
        let dst = create(ptr);
        if dst.is_some() {
            obj.dst = dst;
            Ok(obj)
        } else {
            let ret = Self::end(obj);
            Err(ret)
        }
    }
    /// Returns an immutable reference to the borrower object.
    pub fn get_object(&self) -> &U {
        if let Some(ref dst) = self.dst {
            dst
        } else {
            unreachable!()
        }
    }
    /// Returns a mutable reference to the borrower object.
    pub fn get_object_mut(&mut self) -> &mut U {
        if let Some(ref mut dst) = self.dst {
            dst
        } else {
            unreachable!()
        }
    }
    /// Consumes the self-reference box and returns the original borrowed object.
    pub fn end(mut boxed: SBBox<T, U>) -> T {
        boxed.dst = None;
        unsafe {
            let mut cval = std::mem::MaybeUninit::uninit();
            std::ptr::copy_nonoverlapping(&boxed.bval as *const T, cval.as_mut_ptr(), 1);
            let ret = cval.assume_init();
            let unboxed = Pin::into_inner(boxed);
            dealloc(Box::into_raw(unboxed) as *mut u8, Layout::new::<SelfBorrow<T, U>>());
            ret
        }
    }
}

impl<T, U> Drop for SelfBorrow<T, U> {
    fn drop(&mut self) {
        // borrower should be dropped first
        self.dst = None;
    }
}

/// Handy alias for self-borrowing box
pub type SBBox<T, U> = Pin<Box<SelfBorrow<T, U>>>;
