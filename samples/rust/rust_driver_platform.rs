// SPDX-License-Identifier: GPL-2.0

//! Rust Platform driver sample.

use kernel::{c_str, of, platform, prelude::*};

struct SampleDriver {
    pdev: platform::Device,
}

struct Info(u32);

kernel::of_device_table!(
    OF_TABLE,
    MODULE_OF_TABLE,
    <SampleDriver as platform::Driver>::IdInfo,
    [(of::DeviceId::new(c_str!("test,rust-device")), Info(42))]
);

impl platform::Driver for SampleDriver {
    type IdInfo = Info;
    const OF_ID_TABLE: platform::IdTable<Self::IdInfo> = &OF_TABLE;

    fn probe(pdev: &mut platform::Device, info: Option<&Self::IdInfo>) -> Result<Pin<KBox<Self>>> {
        dev_dbg!(pdev.as_ref(), "Probe Rust Platform driver sample.\n");

        if let Some(info) = info {
            dev_info!(pdev.as_ref(), "Probed with info: '{}'.\n", info.0);
        }

        let dev = pdev.as_ref();
        if let Ok(idx) = dev.property_match_string(c_str!("compatible"), c_str!("test,rust-device"))
        {
            dev_info!(pdev.as_ref(), "matched compatible string idx = {}\n", idx);
        }

        if let Ok(str) = dev.property_read_string(c_str!("compatible"))
        {
            dev_info!(pdev.as_ref(), "compatible string = {:?}\n", str);
        }

        let prop = dev.property_read_bool(c_str!("test,bool-prop"));
        dev_info!(dev, "bool prop is {}\n", prop);

        if dev.property_present(c_str!("test,u32-prop")) {
            dev_info!(dev, "'test,u32-prop' is present\n");
        }

        let prop = dev.property_read::<u32>(c_str!("test,u32-optional-prop"), Some(0x12))?;
        dev_info!(
            dev,
            "'test,u32-optional-prop' is {:#x} (default = {:#x})\n",
            prop,
            0x12
        );

        // Missing property without a default will print an error
        let _ = dev.property_read::<u32>(c_str!("test,u32-required-prop"), None);

        let prop: u32 = dev.property_read(c_str!("test,u32-prop"), None)?;
        dev_info!(dev, "'test,u32-prop' is {:#x}\n", prop);

        let prop: [i16; 4] = dev.property_read_array(c_str!("test,i16-array"), None)?;
        dev_info!(dev, "'test,i16-array' is {:?}\n", prop);
        dev_info!(
            dev,
            "'test,i16-array' length is {}\n",
            dev.property_count_elem::<u16>(c_str!("test,i16-array"))
                .unwrap()
        );

        let prop: KVec<i16> = dev.property_read_array_vec(c_str!("test,i16-array"), 4)?;
        dev_info!(dev, "'test,i16-array' is KVec {:?}\n", prop);

        let drvdata = KBox::new(Self { pdev: pdev.clone() }, GFP_KERNEL)?;

        Ok(drvdata.into())
    }
}

impl Drop for SampleDriver {
    fn drop(&mut self) {
        dev_dbg!(self.pdev.as_ref(), "Remove Rust Platform driver sample.\n");
    }
}

kernel::module_platform_driver! {
    type: SampleDriver,
    name: "rust_driver_platform",
    author: "Danilo Krummrich",
    description: "Rust Platform driver",
    license: "GPL v2",
}
