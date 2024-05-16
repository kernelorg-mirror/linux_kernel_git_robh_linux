// SPDX-License-Identifier: GPL-2.0-only OR MIT
#![recursion_limit = "2048"]

//! Driver for the Apple AGX GPUs found in Apple Silicon SoCs.

mod driver;
mod gem;
mod file;

use kernel::module_platform_driver;

module_platform_driver! {
    type: driver::PanthorDriver,
    name: "panthor",
    author: "Arm",
    license: "Dual MIT/GPL",
}
