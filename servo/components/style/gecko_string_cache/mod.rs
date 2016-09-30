/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#![allow(unsafe_code)]

use gecko_bindings::bindings::Gecko_AddRefAtom;
use gecko_bindings::bindings::Gecko_Atomize;
use gecko_bindings::bindings::Gecko_ReleaseAtom;
use gecko_bindings::structs::nsIAtom;
use heapsize::HeapSizeOf;
use selectors::bloom::BloomHash;
use selectors::parser::FromCowStr;
use std::ascii::AsciiExt;
use std::borrow::{Cow, Borrow};
use std::char::{self, DecodeUtf16};
use std::fmt;
use std::hash::{Hash, Hasher};
use std::iter::Cloned;
use std::mem;
use std::ops::Deref;
use std::slice;

#[macro_use]
#[allow(improper_ctypes, non_camel_case_types)]
pub mod atom_macro;
#[macro_use]
pub mod namespace;

pub use string_cache::namespace::{Namespace, WeakNamespace};

/// A strong reference to a Gecko atom.
#[derive(PartialEq, Eq)]
pub struct Atom(*mut WeakAtom);

/// An atom *without* a strong reference.
///
/// Only usable as `&'a WeakAtom`,
/// where `'a` is the lifetime of something that holds a strong reference to that atom.
pub struct WeakAtom(nsIAtom);

pub type BorrowedAtom<'a> = &'a WeakAtom;

impl Deref for Atom {
    type Target = WeakAtom;

    #[inline]
    fn deref(&self) -> &WeakAtom {
        unsafe {
            &*self.0
        }
    }
}

impl Borrow<WeakAtom> for Atom {
    #[inline]
    fn borrow(&self) -> &WeakAtom {
        self
    }
}

impl Eq for WeakAtom {}
impl PartialEq for WeakAtom {
    #[inline]
    fn eq(&self, other: &Self) -> bool {
        let weak: *const WeakAtom = self;
        let other: *const WeakAtom = other;
        weak == other
    }
}

unsafe impl Send for Atom {}
unsafe impl Sync for Atom {}
unsafe impl Sync for WeakAtom {}

impl WeakAtom {
    #[inline]
    pub unsafe fn new<'a>(atom: *mut nsIAtom) -> &'a mut Self {
        &mut *(atom as *mut WeakAtom)
    }

    #[inline]
    pub fn clone(&self) -> Atom {
        Atom::from(self.as_ptr())
    }

    #[inline]
    pub fn get_hash(&self) -> u32 {
        self.0.mHash
    }

    pub fn as_slice(&self) -> &[u16] {
        unsafe {
            slice::from_raw_parts((*self.as_ptr()).mString, self.len() as usize)
        }
    }

    // NOTE: don't expose this, since it's slow, and easy to be misused.
    fn chars(&self) -> DecodeUtf16<Cloned<slice::Iter<u16>>> {
        char::decode_utf16(self.as_slice().iter().cloned())
    }

    pub fn with_str<F, Output>(&self, cb: F) -> Output
        where F: FnOnce(&str) -> Output
    {
        // FIXME(bholley): We should measure whether it makes more sense to
        // cache the UTF-8 version in the Gecko atom table somehow.
        let owned = String::from_utf16(self.as_slice()).unwrap();
        cb(&owned)
    }

    #[inline]
    pub fn eq_str_ignore_ascii_case(&self, s: &str) -> bool {
        self.chars().map(|r| match r {
            Ok(c) => c.to_ascii_lowercase() as u32,
            Err(e) => e.unpaired_surrogate() as u32,
        }).eq(s.chars().map(|c| c.to_ascii_lowercase() as u32))
    }

    #[inline]
    pub fn to_string(&self) -> String {
        String::from_utf16(self.as_slice()).unwrap()
    }

    #[inline]
    pub fn is_static(&self) -> bool {
        unsafe {
            (*self.as_ptr()).mIsStatic() != 0
        }
    }

    #[inline]
    pub fn len(&self) -> u32 {
        unsafe {
            (*self.as_ptr()).mLength()
        }
    }

    #[inline]
    pub fn as_ptr(&self) -> *mut nsIAtom {
        let const_ptr: *const nsIAtom = &self.0;
        const_ptr as *mut nsIAtom
    }
}

impl fmt::Debug for WeakAtom {
    fn fmt(&self, w: &mut fmt::Formatter) -> fmt::Result {
        write!(w, "Gecko WeakAtom({:p}, {})", self, self)
    }
}

impl fmt::Display for WeakAtom {
    fn fmt(&self, w: &mut fmt::Formatter) -> fmt::Result {
        for c in self.chars() {
            try!(write!(w, "{}", c.unwrap_or(char::REPLACEMENT_CHARACTER)))
        }
        Ok(())
    }
}

impl Atom {
    pub unsafe fn with<F>(ptr: *mut nsIAtom, callback: &mut F) where F: FnMut(&Atom) {
        let atom = Atom(WeakAtom::new(ptr));
        callback(&atom);
        mem::forget(atom);
    }

    /// Creates an atom from an static atom pointer without checking in release
    /// builds.
    ///
    /// Right now it's only used by the atom macro, and ideally it should keep
    /// that way, now we have sugar for is_static, creating atoms using
    /// Atom::from should involve almost no overhead.
    #[inline]
    unsafe fn from_static(ptr: *mut nsIAtom) -> Self {
        let atom = Atom(ptr as *mut WeakAtom);
        debug_assert!(atom.is_static(),
                      "Called from_static for a non-static atom!");
        atom
    }
}

impl BloomHash for Atom {
    #[inline]
    fn bloom_hash(&self) -> u32 {
        self.get_hash()
    }
}

impl BloomHash for WeakAtom {
    #[inline]
    fn bloom_hash(&self) -> u32 {
        self.get_hash()
    }
}

impl Hash for Atom {
    fn hash<H>(&self, state: &mut H) where H: Hasher {
        state.write_u32(self.get_hash());
    }
}

impl Hash for WeakAtom {
    fn hash<H>(&self, state: &mut H) where H: Hasher {
        state.write_u32(self.get_hash());
    }
}

impl Clone for Atom {
    #[inline(always)]
    fn clone(&self) -> Atom {
        Atom::from(self.as_ptr())
    }
}

impl Drop for Atom {
    #[inline]
    fn drop(&mut self) {
        if !self.is_static() {
            unsafe {
                Gecko_ReleaseAtom(self.as_ptr());
            }
        }
    }
}

impl Default for Atom {
    #[inline]
    fn default() -> Self {
        atom!("")
    }
}

impl HeapSizeOf for Atom {
    fn heap_size_of_children(&self) -> usize {
        0
    }
}

impl fmt::Debug for Atom {
    fn fmt(&self, w: &mut fmt::Formatter) -> fmt::Result {
        write!(w, "Gecko Atom({:p}, {})", self.0, self)
    }
}

impl fmt::Display for Atom {
    fn fmt(&self, w: &mut fmt::Formatter) -> fmt::Result {
        unsafe {
            (&*self.0).fmt(w)
        }
    }
}

impl<'a> From<&'a str> for Atom {
    #[inline]
    fn from(string: &str) -> Atom {
        debug_assert!(string.len() <= u32::max_value() as usize);
        unsafe {
            Atom(WeakAtom::new(
                Gecko_Atomize(string.as_ptr() as *const _, string.len() as u32)
            ))
        }
    }
}

impl<'a> From<Cow<'a, str>> for Atom {
    #[inline]
    fn from(string: Cow<'a, str>) -> Atom {
        Atom::from(&*string)
    }
}

impl FromCowStr for Atom {
    #[inline]
    fn from_cow_str(string: Cow<str>) -> Atom {
        Atom::from(&*string)
    }
}

impl From<String> for Atom {
    #[inline]
    fn from(string: String) -> Atom {
        Atom::from(&*string)
    }
}

impl From<*mut nsIAtom> for Atom {
    #[inline]
    fn from(ptr: *mut nsIAtom) -> Atom {
        debug_assert!(!ptr.is_null());
        unsafe {
            if (*ptr).mIsStatic() == 0 {
                Gecko_AddRefAtom(ptr);
            }
            Atom(WeakAtom::new(ptr))
        }
    }
}
