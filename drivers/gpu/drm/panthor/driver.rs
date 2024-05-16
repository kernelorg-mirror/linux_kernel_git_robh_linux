// SPDX-License-Identifier: GPL-2.0-only OR MIT

//! Top-level GPU driver implementation.

use kernel::{
    bindings,
    str::BStr, c_str, drm, drm::drv, drm::ioctl, error::Result,
    of, platform, prelude::*,
    sync::Arc,
};

use kernel::macros::vtable;
use kernel::types::ARef;
use core::ffi;

extern "C" {
    fn panthor_device_init(ptdev: *mut bindings::panthor_device) -> ffi::c_int;
   }

/// Driver metadata
const INFO: drm::drv::DriverInfo = drm::drv::DriverInfo {
    major: 0,
    minor: 0,
    patchlevel: 0,
    name: c_str!("panthor"),
    desc: c_str!("Panthor DRM driver"),
    date: c_str!("20230801"),
};

/// Device data for the driver registration.
///
/// Holds a reference to the top-level `GpuManager` object.
#[allow(dead_code)]
pub(crate) struct PanthorData {
    pub(crate) ptdev: *mut bindings::panthor_device,
    pub(crate) pdev: platform::Device,
}

//HACK
unsafe impl Send for PanthorData {}
unsafe impl Sync for PanthorData {}

/// Empty struct representing this driver.
pub(crate) struct PanthorDriver;

/// Convenience type alias for the DRM device type for this driver.
pub(crate) type PanthorDevice = kernel::drm::device::Device<PanthorDriver>;
pub(crate) type PanthorDevRef = ARef<PanthorDevice>;

/// DRM Driver implementation for `PanthorDriver`.
#[vtable]
impl drv::Driver for PanthorDriver {
    /// Our `DeviceData` type, reference-counted
    type Data = Arc<PanthorData>;
    /// Our `File` type.
    type File = crate::file::File;
    /// Our `Object` type.
    type Object = crate::gem::Object;

    const INFO: drv::DriverInfo = INFO;
    const FEATURES: u32 = drv::FEAT_GEM
        | drv::FEAT_RENDER
        | drv::FEAT_SYNCOBJ
        | drv::FEAT_SYNCOBJ_TIMELINE
        | drv::FEAT_GEM_GPUVA;

    kernel::declare_drm_ioctls! {
        (PANTHOR_DEV_QUERY,      drm_panthor_dev_query,
            ioctl::RENDER_ALLOW, crate::file::File::dev_query),
        (PANTHOR_VM_CREATE,      drm_panthor_vm_create,
            ioctl::RENDER_ALLOW, crate::file::File::vm_create),
        (PANTHOR_VM_DESTROY,     drm_panthor_vm_destroy,
            ioctl::RENDER_ALLOW, crate::file::File::vm_destroy),
        (PANTHOR_VM_BIND,        drm_panthor_vm_bind,
            ioctl::RENDER_ALLOW, crate::file::File::vm_bind),
        (PANTHOR_VM_GET_STATE,   drm_panthor_vm_get_state,
            ioctl::RENDER_ALLOW, crate::file::File::vm_get_state),
        (PANTHOR_BO_CREATE,      drm_panthor_bo_create,
            ioctl::RENDER_ALLOW, crate::file::File::bo_create),
        (PANTHOR_BO_MMAP_OFFSET, drm_panthor_bo_mmap_offset,
            ioctl::RENDER_ALLOW, crate::file::File::bo_mmap_offset),
        (PANTHOR_GROUP_CREATE,   drm_panthor_group_create,
            ioctl::RENDER_ALLOW, crate::file::File::group_create),
        (PANTHOR_GROUP_DESTROY,  drm_panthor_group_destroy,
            ioctl::RENDER_ALLOW, crate::file::File::group_destroy),
        (PANTHOR_GROUP_SUBMIT,   drm_panthor_group_submit,
            ioctl::RENDER_ALLOW, crate::file::File::group_submit),
        (PANTHOR_GROUP_GET_STATE, drm_panthor_group_get_state,
            ioctl::RENDER_ALLOW, crate::file::File::group_get_state),
        (PANTHOR_TILER_HEAP_CREATE, drm_panthor_tiler_heap_create,
            ioctl::RENDER_ALLOW, crate::file::File::tiler_heap_create),
        (PANTHOR_TILER_HEAP_DESTROY, drm_panthor_tiler_heap_destroy,
            ioctl::RENDER_ALLOW, crate::file::File::tiler_heap_destroy),
    }
}

/// Platform Driver implementation for `PanthorDriver`.
impl platform::Driver for PanthorDriver {
    /// Our `DeviceData` type, reference-counted
    type Data = Arc<PanthorData>;

    // Assign the above OF ID table to this driver.
    kernel::define_of_id_table! { (), [
        (of::DeviceId(BStr::from_bytes(b"rockchip,rk3588-mali")), None),
        (of::DeviceId(BStr::from_bytes(b"arm,mali-valhall-csf")), None),]
    }

    /// Device probe function.
    fn probe(
        pdev: &mut platform::Device,
        id_info: Option<&Self::IdInfo>,
    ) -> Result<Self::Data> {

        dev_info!(pdev.as_ref(), "Probing...\n");

        let data = Arc::new(
            PanthorData {
                ptdev: unsafe { bindings::panthor_device_alloc() },
                pdev: pdev.clone(),
            },
            GFP_KERNEL,
        )?;

//        let mut ptdev: bindings::panthor_device = Default::default();

        let drm = drm::device::Device::<PanthorDriver>::new(pdev.as_ref(), data.clone())?;

        unsafe {
            (*data.ptdev).base = drm.as_raw();
            panthor_device_init(data.ptdev);
        }

        drm::drv::Registration::new_foreign_owned(drm, 0)?;

        dev_info!(pdev.as_ref(), "Probed!\n");
        Ok(data)
    }
}

// Export the OF ID table as a module ID table, to make modpost/autoloading work.
//kernel::module_of_id_table!(MOD_TABLE, PANTHOR_ID_TABLE);

