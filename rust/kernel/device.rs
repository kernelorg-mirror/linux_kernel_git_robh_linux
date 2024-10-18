// SPDX-License-Identifier: GPL-2.0

//! Generic devices that are part of the kernel's driver model.
//!
//! C header: [`include/linux/device.h`](srctree/include/linux/device.h)

use crate::{
    alloc::KVec,
    bindings,
    error::{to_result, Result},
    prelude::*,
    str::{CStr, CString},
    types::{ARef, Integer, Opaque},
};
use core::{ffi::c_void, fmt, mem, mem::MaybeUninit, ptr};

#[cfg(CONFIG_PRINTK)]
use crate::c_str;

/// A reference-counted device.
///
/// This structure represents the Rust abstraction for a C `struct device`. This implementation
/// abstracts the usage of an already existing C `struct device` within Rust code that we get
/// passed from the C side.
///
/// An instance of this abstraction can be obtained temporarily or permanent.
///
/// A temporary one is bound to the lifetime of the C `struct device` pointer used for creation.
/// A permanent instance is always reference-counted and hence not restricted by any lifetime
/// boundaries.
///
/// For subsystems it is recommended to create a permanent instance to wrap into a subsystem
/// specific device structure (e.g. `pci::Device`). This is useful for passing it to drivers in
/// `T::probe()`, such that a driver can store the `ARef<Device>` (equivalent to storing a
/// `struct device` pointer in a C driver) for arbitrary purposes, e.g. allocating DMA coherent
/// memory.
///
/// # Invariants
///
/// A `Device` instance represents a valid `struct device` created by the C portion of the kernel.
///
/// Instances of this type are always reference-counted, that is, a call to `get_device` ensures
/// that the allocation remains valid at least until the matching call to `put_device`.
///
/// `bindings::device::release` is valid to be called from any thread, hence `ARef<Device>` can be
/// dropped from any thread.
#[repr(transparent)]
pub struct Device(Opaque<bindings::device>);

impl Device {
    /// Creates a new reference-counted abstraction instance of an existing `struct device` pointer.
    ///
    /// # Safety
    ///
    /// Callers must ensure that `ptr` is valid, non-null, and has a non-zero reference count,
    /// i.e. it must be ensured that the reference count of the C `struct device` `ptr` points to
    /// can't drop to zero, for the duration of this function call.
    ///
    /// It must also be ensured that `bindings::device::release` can be called from any thread.
    /// While not officially documented, this should be the case for any `struct device`.
    pub unsafe fn get_device(ptr: *mut bindings::device) -> ARef<Self> {
        // SAFETY: By the safety requirements ptr is valid
        unsafe { Self::as_ref(ptr) }.into()
    }

    /// Obtain the raw `struct device *`.
    pub(crate) fn as_raw(&self) -> *mut bindings::device {
        self.0.get()
    }

    /// Convert a raw C `struct device` pointer to a `&'a Device`.
    ///
    /// # Safety
    ///
    /// Callers must ensure that `ptr` is valid, non-null, and has a non-zero reference count,
    /// i.e. it must be ensured that the reference count of the C `struct device` `ptr` points to
    /// can't drop to zero, for the duration of this function call and the entire duration when the
    /// returned reference exists.
    pub unsafe fn as_ref<'a>(ptr: *mut bindings::device) -> &'a Self {
        // SAFETY: Guaranteed by the safety requirements of the function.
        unsafe { &*ptr.cast() }
    }

    /// Prints an emergency-level message (level 0) prefixed with device information.
    ///
    /// More details are available from [`dev_emerg`].
    ///
    /// [`dev_emerg`]: crate::dev_emerg
    pub fn pr_emerg(&self, args: fmt::Arguments<'_>) {
        // SAFETY: `klevel` is null-terminated, uses one of the kernel constants.
        unsafe { self.printk(bindings::KERN_EMERG, args) };
    }

    /// Prints an alert-level message (level 1) prefixed with device information.
    ///
    /// More details are available from [`dev_alert`].
    ///
    /// [`dev_alert`]: crate::dev_alert
    pub fn pr_alert(&self, args: fmt::Arguments<'_>) {
        // SAFETY: `klevel` is null-terminated, uses one of the kernel constants.
        unsafe { self.printk(bindings::KERN_ALERT, args) };
    }

    /// Prints a critical-level message (level 2) prefixed with device information.
    ///
    /// More details are available from [`dev_crit`].
    ///
    /// [`dev_crit`]: crate::dev_crit
    pub fn pr_crit(&self, args: fmt::Arguments<'_>) {
        // SAFETY: `klevel` is null-terminated, uses one of the kernel constants.
        unsafe { self.printk(bindings::KERN_CRIT, args) };
    }

    /// Prints an error-level message (level 3) prefixed with device information.
    ///
    /// More details are available from [`dev_err`].
    ///
    /// [`dev_err`]: crate::dev_err
    pub fn pr_err(&self, args: fmt::Arguments<'_>) {
        // SAFETY: `klevel` is null-terminated, uses one of the kernel constants.
        unsafe { self.printk(bindings::KERN_ERR, args) };
    }

    /// Prints a warning-level message (level 4) prefixed with device information.
    ///
    /// More details are available from [`dev_warn`].
    ///
    /// [`dev_warn`]: crate::dev_warn
    pub fn pr_warn(&self, args: fmt::Arguments<'_>) {
        // SAFETY: `klevel` is null-terminated, uses one of the kernel constants.
        unsafe { self.printk(bindings::KERN_WARNING, args) };
    }

    /// Prints a notice-level message (level 5) prefixed with device information.
    ///
    /// More details are available from [`dev_notice`].
    ///
    /// [`dev_notice`]: crate::dev_notice
    pub fn pr_notice(&self, args: fmt::Arguments<'_>) {
        // SAFETY: `klevel` is null-terminated, uses one of the kernel constants.
        unsafe { self.printk(bindings::KERN_NOTICE, args) };
    }

    /// Prints an info-level message (level 6) prefixed with device information.
    ///
    /// More details are available from [`dev_info`].
    ///
    /// [`dev_info`]: crate::dev_info
    pub fn pr_info(&self, args: fmt::Arguments<'_>) {
        // SAFETY: `klevel` is null-terminated, uses one of the kernel constants.
        unsafe { self.printk(bindings::KERN_INFO, args) };
    }

    /// Prints a debug-level message (level 7) prefixed with device information.
    ///
    /// More details are available from [`dev_dbg`].
    ///
    /// [`dev_dbg`]: crate::dev_dbg
    pub fn pr_dbg(&self, args: fmt::Arguments<'_>) {
        if cfg!(debug_assertions) {
            // SAFETY: `klevel` is null-terminated, uses one of the kernel constants.
            unsafe { self.printk(bindings::KERN_DEBUG, args) };
        }
    }

    /// Prints the provided message to the console.
    ///
    /// # Safety
    ///
    /// Callers must ensure that `klevel` is null-terminated; in particular, one of the
    /// `KERN_*`constants, for example, `KERN_CRIT`, `KERN_ALERT`, etc.
    #[cfg_attr(not(CONFIG_PRINTK), allow(unused_variables))]
    unsafe fn printk(&self, klevel: &[u8], msg: fmt::Arguments<'_>) {
        // SAFETY: `klevel` is null-terminated and one of the kernel constants. `self.as_raw`
        // is valid because `self` is valid. The "%pA" format string expects a pointer to
        // `fmt::Arguments`, which is what we're passing as the last argument.
        #[cfg(CONFIG_PRINTK)]
        unsafe {
            bindings::_dev_printk(
                klevel as *const _ as *const core::ffi::c_char,
                self.as_raw(),
                c_str!("%pA").as_char_ptr(),
                &msg as *const _ as *const core::ffi::c_void,
            )
        };
    }

    /// Returns if a firmware property `name` is present
    pub fn property_present(&self, name: &CStr) -> bool {
        // SAFETY: `name` is non-null and null-terminated. `self.as_raw` is valid
        // because `self` is valid.
        unsafe { bindings::device_property_present(self.as_raw(), name.as_ptr() as *const i8) }
    }

    /// Returns if a firmware property `name` is true or false
    pub fn property_read_bool(&self, name: &CStr) -> bool {
        // TODO: replace with device_property_read_bool() which warns on non-bool properties
        // SAFETY: `name` is non-null and null-terminated. `self.as_raw` is valid
        // because `self` is valid.
        unsafe { bindings::device_property_present(self.as_raw(), name.as_ptr() as *const i8) }
    }

    /// Returns the index of matching string `match_str` for firmware string property `name`
    pub fn property_read_string(&self, name: &CStr) -> Result<CString> {
        let mut str: *mut i8 = core::ptr::null_mut();
        let pstr: *mut *mut i8 = &mut str;

        // SAFETY: `name` is non-null and null-terminated. `self.as_raw` is
        // valid because `self` is valid.
        let ret = unsafe {
            bindings::device_property_read_string(
                self.as_raw(),
                name.as_ptr() as *const i8,
                pstr as _,
            )
        };
        to_result(ret)?;

        // SAFETY: `pstr` contains a non-null ptr on success
        let str = unsafe { CStr::from_char_ptr(*pstr) };
        Ok(str.try_into()?)
    }


    /// Returns the index of matching string `match_str` for firmware string property `name`
    pub fn property_match_string(&self, name: &CStr, match_str: &CStr) -> Result<usize> {
        // SAFETY: `name` and `match_str` are non-null and null-terminated. `self.as_raw` is
        // valid because `self` is valid.
        let ret = unsafe {
            bindings::device_property_match_string(
                self.as_raw(),
                name.as_ptr() as *const i8,
                match_str.as_ptr() as *const i8,
            )
        };
        to_result(ret)?;
        Ok(ret as usize)
    }

    /// Returns firmware property `name` integer scalar value
    pub fn property_read<T: Integer>(&self, name: &CStr, default: Option<T>) -> Result<T> {
        let default = default.map(|default| [default; 1]);

        let val = Self::property_read_array(self, name, default)?;
        Ok(val[0])
    }

    /// Returns firmware property `name` integer array values
    pub fn property_read_array<T: Integer, const N: usize>(
        &self,
        name: &CStr,
        default: Option<[T; N]>,
    ) -> Result<[T; N]> {
        let val: [MaybeUninit<T>; N] = [const { MaybeUninit::uninit() }; N];

        // SAFETY: `name` is non-null and null-terminated. `self.as_raw` is valid
        // because `self` is valid. `val.as_ptr` is valid because `val` is valid.
        let ret = unsafe {
            bindings::device_property_read_int_array(
                self.as_raw(),
                name.as_ptr() as *const i8,
                T::SIZE.try_into().unwrap(),
                val.as_ptr() as *mut c_void,
                val.len(),
            )
        };

        let val: [T; N] = match ret {
            // SAFETY: `val` is always initialized when device_property_read_int_array
            // is successful.
            0 => unsafe { mem::transmute_copy(&val) },
            _ => match default {
                Some(default) => default,
                None => {
                    dev_err!(
                        self,
                        "'{}' property is missing and no default given.\n",
                        name
                    );
                    return Err(Error::from_errno(ret));
                }
            },
        };
        Ok(val)
    }

    /// Returns firmware property `name` integer array values in a KVec
    pub fn property_read_array_vec<T: Integer>(&self, name: &CStr, len: usize) -> Result<KVec<T>> {
        let mut val: KVec<T> = KVec::with_capacity(len, GFP_KERNEL)?;

        // SAFETY: `name` is non-null and null-terminated. `self.as_raw` is valid
        // because `self` is valid. `val.as_ptr` is valid because `val` is valid.
        to_result(unsafe {
            bindings::device_property_read_int_array(
                self.as_raw(),
                name.as_ptr() as *const i8,
                T::SIZE.try_into().unwrap(),
                val.as_ptr() as *mut c_void,
                len,
            )
        })?;

        // SAFETY: device_property_read_int_array() writes exactly `len` entries on success
        unsafe { val.set_len(len) }
        Ok(val)
    }

    /// Returns integer array length for firmware property `name`
    pub fn property_count_elem<T: Integer>(&self, name: &CStr) -> Result<usize> {
        // SAFETY: `name` is non-null and null-terminated. `self.as_raw` is valid
        // because `self` is valid. Passing null pointer buffer is valid to obtain
        // the number of elements in the property array.
        let ret = unsafe {
            bindings::device_property_read_int_array(
                self.as_raw(),
                name.as_ptr() as *const i8,
                T::SIZE.try_into().unwrap(),
                ptr::null_mut(),
                0,
            )
        };
        to_result(ret)?;
        Ok(ret.try_into().unwrap())
    }
}

// SAFETY: Instances of `Device` are always reference-counted.
unsafe impl crate::types::AlwaysRefCounted for Device {
    fn inc_ref(&self) {
        // SAFETY: The existence of a shared reference guarantees that the refcount is non-zero.
        unsafe { bindings::get_device(self.as_raw()) };
    }

    unsafe fn dec_ref(obj: ptr::NonNull<Self>) {
        // SAFETY: The safety requirements guarantee that the refcount is non-zero.
        unsafe { bindings::put_device(obj.cast().as_ptr()) }
    }
}

// SAFETY: As by the type invariant `Device` can be sent to any thread.
unsafe impl Send for Device {}

// SAFETY: `Device` can be shared among threads because all immutable methods are protected by the
// synchronization in `struct device`.
unsafe impl Sync for Device {}

#[doc(hidden)]
#[macro_export]
macro_rules! dev_printk {
    ($method:ident, $dev:expr, $($f:tt)*) => {
        {
            ($dev).$method(core::format_args!($($f)*));
        }
    }
}

/// Prints an emergency-level message (level 0) prefixed with device information.
///
/// This level should be used if the system is unusable.
///
/// Equivalent to the kernel's `dev_emerg` macro.
///
/// Mimics the interface of [`std::print!`]. More information about the syntax is available from
/// [`core::fmt`] and `alloc::format!`.
///
/// [`std::print!`]: https://doc.rust-lang.org/std/macro.print.html
///
/// # Examples
///
/// ```
/// # use kernel::device::Device;
///
/// fn example(dev: &Device) {
///     dev_emerg!(dev, "hello {}\n", "there");
/// }
/// ```
#[macro_export]
macro_rules! dev_emerg {
    ($($f:tt)*) => { $crate::dev_printk!(pr_emerg, $($f)*); }
}

/// Prints an alert-level message (level 1) prefixed with device information.
///
/// This level should be used if action must be taken immediately.
///
/// Equivalent to the kernel's `dev_alert` macro.
///
/// Mimics the interface of [`std::print!`]. More information about the syntax is available from
/// [`core::fmt`] and `alloc::format!`.
///
/// [`std::print!`]: https://doc.rust-lang.org/std/macro.print.html
///
/// # Examples
///
/// ```
/// # use kernel::device::Device;
///
/// fn example(dev: &Device) {
///     dev_alert!(dev, "hello {}\n", "there");
/// }
/// ```
#[macro_export]
macro_rules! dev_alert {
    ($($f:tt)*) => { $crate::dev_printk!(pr_alert, $($f)*); }
}

/// Prints a critical-level message (level 2) prefixed with device information.
///
/// This level should be used in critical conditions.
///
/// Equivalent to the kernel's `dev_crit` macro.
///
/// Mimics the interface of [`std::print!`]. More information about the syntax is available from
/// [`core::fmt`] and `alloc::format!`.
///
/// [`std::print!`]: https://doc.rust-lang.org/std/macro.print.html
///
/// # Examples
///
/// ```
/// # use kernel::device::Device;
///
/// fn example(dev: &Device) {
///     dev_crit!(dev, "hello {}\n", "there");
/// }
/// ```
#[macro_export]
macro_rules! dev_crit {
    ($($f:tt)*) => { $crate::dev_printk!(pr_crit, $($f)*); }
}

/// Prints an error-level message (level 3) prefixed with device information.
///
/// This level should be used in error conditions.
///
/// Equivalent to the kernel's `dev_err` macro.
///
/// Mimics the interface of [`std::print!`]. More information about the syntax is available from
/// [`core::fmt`] and `alloc::format!`.
///
/// [`std::print!`]: https://doc.rust-lang.org/std/macro.print.html
///
/// # Examples
///
/// ```
/// # use kernel::device::Device;
///
/// fn example(dev: &Device) {
///     dev_err!(dev, "hello {}\n", "there");
/// }
/// ```
#[macro_export]
macro_rules! dev_err {
    ($($f:tt)*) => { $crate::dev_printk!(pr_err, $($f)*); }
}

/// Prints a warning-level message (level 4) prefixed with device information.
///
/// This level should be used in warning conditions.
///
/// Equivalent to the kernel's `dev_warn` macro.
///
/// Mimics the interface of [`std::print!`]. More information about the syntax is available from
/// [`core::fmt`] and `alloc::format!`.
///
/// [`std::print!`]: https://doc.rust-lang.org/std/macro.print.html
///
/// # Examples
///
/// ```
/// # use kernel::device::Device;
///
/// fn example(dev: &Device) {
///     dev_warn!(dev, "hello {}\n", "there");
/// }
/// ```
#[macro_export]
macro_rules! dev_warn {
    ($($f:tt)*) => { $crate::dev_printk!(pr_warn, $($f)*); }
}

/// Prints a notice-level message (level 5) prefixed with device information.
///
/// This level should be used in normal but significant conditions.
///
/// Equivalent to the kernel's `dev_notice` macro.
///
/// Mimics the interface of [`std::print!`]. More information about the syntax is available from
/// [`core::fmt`] and `alloc::format!`.
///
/// [`std::print!`]: https://doc.rust-lang.org/std/macro.print.html
///
/// # Examples
///
/// ```
/// # use kernel::device::Device;
///
/// fn example(dev: &Device) {
///     dev_notice!(dev, "hello {}\n", "there");
/// }
/// ```
#[macro_export]
macro_rules! dev_notice {
    ($($f:tt)*) => { $crate::dev_printk!(pr_notice, $($f)*); }
}

/// Prints an info-level message (level 6) prefixed with device information.
///
/// This level should be used for informational messages.
///
/// Equivalent to the kernel's `dev_info` macro.
///
/// Mimics the interface of [`std::print!`]. More information about the syntax is available from
/// [`core::fmt`] and `alloc::format!`.
///
/// [`std::print!`]: https://doc.rust-lang.org/std/macro.print.html
///
/// # Examples
///
/// ```
/// # use kernel::device::Device;
///
/// fn example(dev: &Device) {
///     dev_info!(dev, "hello {}\n", "there");
/// }
/// ```
#[macro_export]
macro_rules! dev_info {
    ($($f:tt)*) => { $crate::dev_printk!(pr_info, $($f)*); }
}

/// Prints a debug-level message (level 7) prefixed with device information.
///
/// This level should be used for debug messages.
///
/// Equivalent to the kernel's `dev_dbg` macro, except that it doesn't support dynamic debug yet.
///
/// Mimics the interface of [`std::print!`]. More information about the syntax is available from
/// [`core::fmt`] and `alloc::format!`.
///
/// [`std::print!`]: https://doc.rust-lang.org/std/macro.print.html
///
/// # Examples
///
/// ```
/// # use kernel::device::Device;
///
/// fn example(dev: &Device) {
///     dev_dbg!(dev, "hello {}\n", "there");
/// }
/// ```
#[macro_export]
macro_rules! dev_dbg {
    ($($f:tt)*) => { $crate::dev_printk!(pr_dbg, $($f)*); }
}
