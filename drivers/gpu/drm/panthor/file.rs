// SPDX-License-Identifier: GPL-2.0-only OR MIT
#![allow(clippy::unusual_byte_groupings)]

//! File implementation, which represents a single DRM client.
//!
//! This is in charge of managing the resources associated with one GPU client, including an
//! arbitrary number of submission queues and Vm objects, and reporting hardware/driver
//! information to userspace and accepting submissions.

//use crate::debug::*;
use crate::driver::PanthorDevice;
use crate::driver::PanthorDriver;
//use crate::{alloc, buffer, driver, gem, mmu, queue, util::RangeExt};
//use kernel::drm::gem::BaseObject;
//use kernel::io_buffer::{IoBufferReader, IoBufferWriter};
use kernel::prelude::*;
use kernel::{
    drm::{self, device::Device as DrmDevice},
    drm::file::GenericFile,
    uapi,
    error::to_result,
    bindings,
};
use core;
use core::ptr;
use core::ffi;

/// Convenience type alias for our DRM `File` type.
pub(crate) type DrmFile = drm::file::File<File>;

extern "C" {
    fn _panthor_ioctl_dev_query(ptdev: *mut bindings::panthor_device, args: *mut uapi::drm_panthor_dev_query) -> ffi::c_int;
    fn _panthor_ioctl_vm_create(ptdev: *mut bindings::panthor_device, data: *mut uapi::drm_panthor_vm_create, file: *const bindings::drm_file) -> ffi::c_int;
    fn panthor_ioctl_vm_bind_async(args: *mut uapi::drm_panthor_vm_bind, file: *mut bindings::drm_file) -> ffi::c_int;
    fn panthor_ioctl_vm_bind_sync(args: *mut uapi::drm_panthor_vm_bind, file: *mut bindings::drm_file) -> ffi::c_int;
    fn panthor_group_submit(
        ddev: *mut bindings::drm_device,
        args: *mut uapi::drm_panthor_group_submit,
        file: *mut bindings::drm_file,
    ) -> core::ffi::c_int;
    fn panthor_tiler_heap_create(
        pfile: *mut bindings::panthor_file,
        args: *mut uapi::drm_panthor_tiler_heap_create,
    ) -> core::ffi::c_int;
    fn panthor_tiler_heap_destroy(
        pfile: *mut bindings::panthor_file,
        args: *const uapi::drm_panthor_tiler_heap_destroy,
    ) -> core::ffi::c_int;
    fn _panthor_group_create(
        args: *mut uapi::drm_panthor_group_create,
        pfile: *mut bindings::panthor_file,
    ) -> core::ffi::c_int;
    fn panthor_group_get_state(
        pfile: *mut bindings::panthor_file,
        get_state: *mut uapi::drm_panthor_group_get_state,
    ) -> core::ffi::c_int;
}

/// State associated with a client.
pub(crate) struct File();

impl drm::file::DriverFile for File {
    type Driver = PanthorDriver;

    fn open(_dev: &DrmDevice<Self::Driver>) -> Result<Pin<Box<Self>>> {
        pr_info!("DRM Device :: open()\n");

        Ok(Box::into_pin(Box::new(Self(), GFP_KERNEL)?))
    }
}

impl File {
    /// IOCTL: dev_query
    pub(crate) fn dev_query(
        device: &PanthorDevice,
        data: &mut uapi::drm_panthor_dev_query,
        _file: &DrmFile,
    ) -> Result<u32> {
        let devdata = device.data();

        to_result(unsafe {
            _panthor_ioctl_dev_query(devdata.ptdev, data)
        })?;
        Ok(0)
    }

    /// IOCTL: vm_create
    pub(crate) fn vm_create(
        device: &PanthorDevice,
        data: &mut uapi::drm_panthor_vm_create,
        file: &DrmFile,
    ) -> Result<u32> {
        let devdata = device.data();

        to_result(unsafe {
            _panthor_ioctl_vm_create(devdata.ptdev, data, file.raw())
        })?;
        Ok(0)
    }

    /// IOCTL: vm_destroy
    pub(crate) fn vm_destroy(
        _device: &PanthorDevice,
        data: &mut uapi::drm_panthor_vm_destroy,
        file: &DrmFile,
    ) -> Result<u32> {
        if data.pad != 0 {
            return Err(EINVAL);
        }

        to_result(unsafe {
            let pfile: *mut bindings::panthor_file = (*file.raw()).driver_priv as *mut _;
            bindings::panthor_vm_pool_destroy_vm((*pfile).vms, data.id)
        })?;
        Ok(0)
    }

    /// IOCTL: vm_bind
    pub(crate) fn vm_bind(
        _device: &PanthorDevice,
        data: &mut uapi::drm_panthor_vm_bind,
        file: &DrmFile,
    ) -> Result<u32> {
        //TODO: drm_dev_enter(device., &cookie))

        if data.flags == uapi::drm_panthor_vm_bind_flags_DRM_PANTHOR_VM_BIND_ASYNC {
            to_result(unsafe {panthor_ioctl_vm_bind_async(data, file.raw() as *mut _)})?;
        } else {
            to_result(unsafe {panthor_ioctl_vm_bind_sync(data, file.raw() as *mut _)})?;
        }
        Ok(0)
    }

    /// IOCTL: vm_get_state
    pub(crate) fn vm_get_state(
        _device: &PanthorDevice,
        data: &mut uapi::drm_panthor_vm_get_state,
        file: &DrmFile,
    ) -> Result<u32> {
        let pfile: *mut bindings::panthor_file = unsafe {(*file.raw()).driver_priv as *mut _};
        let vm = unsafe { bindings::panthor_vm_pool_get_vm((*pfile).vms, data.vm_id) };

        if vm.is_null() {
            return Err(EINVAL);
        }

        if unsafe {bindings::panthor_vm_is_unusable(vm)} {
            data.state = uapi::drm_panthor_vm_state_DRM_PANTHOR_VM_STATE_UNUSABLE;
        } else {
            data.state = uapi::drm_panthor_vm_state_DRM_PANTHOR_VM_STATE_USABLE;
        }

        unsafe {bindings::panthor_vm_put(vm)};
        Ok(0)
    }

    /// IOCTL: bo_create
    pub(crate) fn bo_create(
        device: &PanthorDevice,
        data: &mut uapi::drm_panthor_bo_create,
        file: &DrmFile,
    ) -> Result<u32> {
        //TODO if (!drm_dev_enter(ddev, &cookie))
        //    return -ENODEV;

    if data.size == 0 || data.pad != 0 ||
       (data.flags & !uapi::drm_panthor_bo_flags_DRM_PANTHOR_BO_NO_MMAP != 0) {
            return Err(EINVAL);
        }

        let mut vm = ptr::null_mut();
        if data.exclusive_vm_id != 0 {
            let pfile: *mut bindings::panthor_file = unsafe {(*file.raw()).driver_priv as *mut _};
            vm = unsafe { bindings::panthor_vm_pool_get_vm((*pfile).vms, data.exclusive_vm_id) };

            if vm.is_null() {
                return Err(EINVAL);
            }
        }

        let ret = unsafe {
            let ptdev = device.data().ptdev;
            bindings::panthor_gem_create_with_handle(file.raw() as *mut _, (*ptdev).base,
                vm, &mut data.size as *mut _, data.flags, &mut data.handle as *mut _)
        };
        unsafe {bindings::panthor_vm_put(vm)};

        to_result(ret)?;
        Ok(0)
    }

    /// IOCTL: bo_mmap_offset
    pub(crate) fn bo_mmap_offset(
        _device: &PanthorDevice,
        _data: &mut uapi::drm_panthor_bo_mmap_offset,
        _file: &DrmFile,
    ) -> Result<u32> {
        Ok(0)
    }

    /// IOCTL: group_create
    pub(crate) fn group_create(
        _device: &PanthorDevice,
        data: &mut uapi::drm_panthor_group_create,
        file: &DrmFile,
    ) -> Result<u32> {
        let pfile: *mut bindings::panthor_file = unsafe {(*file.raw()).driver_priv as *mut _};

        to_result(unsafe {_panthor_group_create(data, pfile)})?;
        Ok(0)
    }

    /// IOCTL: group_destroy
    pub(crate) fn group_destroy(
        _device: &PanthorDevice,
        data: &mut uapi::drm_panthor_group_destroy,
        file: &DrmFile,
    ) -> Result<u32> {
        if data.pad != 0 {
            return Err(EINVAL)
        }

        let pfile: *mut bindings::panthor_file = unsafe {(*file.raw()).driver_priv as *mut _};

        to_result(unsafe {bindings::panthor_group_destroy(pfile, data.group_handle)})?;
        Ok(0)
    }

    /// IOCTL: group_get_state
    pub(crate) fn group_get_state(
        _device: &PanthorDevice,
        data: &mut uapi::drm_panthor_group_get_state,
        file: &DrmFile,
    ) -> Result<u32> {
        let pfile: *mut bindings::panthor_file = unsafe {(*file.raw()).driver_priv as *mut _};

        to_result(unsafe {panthor_group_get_state(pfile, data)})?;
        Ok(0)
    }

    /// IOCTL: tiler_heap_create
    pub(crate) fn tiler_heap_create(
        _device: &PanthorDevice,
        data: &mut uapi::drm_panthor_tiler_heap_create,
        file: &DrmFile,
    ) -> Result<u32> {
        let pfile: *mut bindings::panthor_file = unsafe {(*file.raw()).driver_priv as *mut _};

        to_result(unsafe {panthor_tiler_heap_create(pfile, data)})?;
        Ok(0)
    }

    /// IOCTL: tiler_heap_destroy
    pub(crate) fn tiler_heap_destroy(
        _device: &PanthorDevice,
        data: &mut uapi::drm_panthor_tiler_heap_destroy,
        file: &DrmFile,
    ) -> Result<u32> {
        let pfile: *mut bindings::panthor_file = unsafe {(*file.raw()).driver_priv as *mut _};

        to_result(unsafe {panthor_tiler_heap_destroy(pfile, data)})?;
        Ok(0)
    }

    /// IOCTL: group_submit
    pub(crate) fn group_submit(
        device: &PanthorDevice,
        data: &mut uapi::drm_panthor_group_submit,
        file: &DrmFile,
    ) -> Result<u32> {
        to_result(unsafe {
            let ptdev = device.data().ptdev;
            panthor_group_submit((*ptdev).base, data, file.raw() as *mut _)
        })?;
        Ok(0)
    }
}

impl Drop for File {
    fn drop(&mut self) {
    }
}
