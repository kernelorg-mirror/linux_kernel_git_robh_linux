use core::sync::atomic::{AtomicU64, Ordering};

use kernel::{drm::gem, prelude::*};

use crate::driver::{PanthorDevice, PanthorDriver};

static GEM_ID: AtomicU64 = AtomicU64::new(0);

/// GEM Object implementation
#[pin_data]
pub(crate) struct DriverObject {
    /// ID for debugging
    id: u64,
}

pub(crate) type Object = gem::Object<DriverObject>;

impl gem::BaseDriverObject<Object> for DriverObject {
    fn new(_dev: &PanthorDevice, _size: usize) -> impl PinInit<Self, Error> {
        let id = GEM_ID.fetch_add(1, Ordering::Relaxed);

        pr_debug!("DriverObject::new() id={}\n", id);
        DriverObject { id }
    }
}

impl gem::DriverObject for DriverObject {
    type Driver = PanthorDriver;
}
