use kernel::{
    alloc::{flags::*, Flags},
    devres::Devres,
    pci,
    prelude::*,
    revocable::RevocableGuard,
};

use crate::fwsec::FalconUCodeDescV3;

const PROM_OFFSET: usize = 0x300000;

#[repr(C)]
#[derive(Default)]
struct BitEntry {
    id: u8,
    version: u8,
    length: u16,
    offset: u16,
}

#[allow(dead_code)]
#[derive(Default)]
struct BiosPcirT {
    vendor_id: u16,
    device_id: u16,
    class_code: [u8; 3],
    image_size: u32,
    image_rev: u16,
    image_type: u8,
    last: bool,
}

#[derive(Default)]
struct BiosNpdeT {
    image_size: u32,
    last: bool,
}

#[derive(Default)]
struct BiosPmuE {
    pmutype: u8,
    data: u32,
}

#[derive(Default)]
struct BiosImage {
    base: usize,
    size: usize,
    itype: u8,
    last: bool,
}

pub(crate) struct Bios {
    image0_size: isize,
    imaged_addr: usize,

    bmp_offset: usize,
    bit_offset: usize,

    bios_vec: Vec<u8>,
    bios_vec_gfp: Flags,
}

impl Bios {
    pub(crate) fn new() -> Self {
        Self {
            image0_size: 0,
            imaged_addr: 0,
            bmp_offset: 0,
            bit_offset: 0,
            bios_vec: Default::default(),
            bios_vec_gfp: GFP_KERNEL | __GFP_ZERO,
        }
    }

    fn rd32(&self, offset: isize) -> u32 {
        let mut addr = offset;
        if addr >= self.image0_size && self.imaged_addr != 0 {
            addr -= self.image0_size;
            addr += self.imaged_addr as isize;
        }
        let ptr: *const u32 = (self.bios_vec.as_ptr() as usize + addr as usize) as *const u32;
        unsafe { core::ptr::read_unaligned(ptr) }
    }

    fn rd16(&self, offset: isize) -> u16 {
        let mut addr = offset;
        if addr >= self.image0_size && self.imaged_addr != 0 {
            addr -= self.image0_size;
            addr += self.imaged_addr as isize;
        }
        let ptr: *const u16 = (self.bios_vec.as_ptr() as usize + addr as usize) as *const u16;
        unsafe { core::ptr::read_unaligned(ptr) }
    }

    fn rd08(&self, offset: isize) -> u8 {
        let mut addr = offset;
        if addr >= self.image0_size && self.imaged_addr != 0 {
            addr -= self.image0_size;
            addr += self.imaged_addr as isize;
        }
        let ptr: *const u8 = self.bios_vec.as_ptr();
        unsafe { *(ptr.offset(addr)) }
    }

    fn ptr(&self, offset: isize) -> *const u8 {
        let mut addr = offset;
        if addr >= self.image0_size && self.imaged_addr != 0 {
            addr -= self.image0_size;
            addr += self.imaged_addr as isize;
        }
        (self.bios_vec.as_ptr() as usize + addr as usize) as *const u8
    }

    fn findbytes(&self, needle: &[u8]) -> usize {
        for i in 0..self.bios_vec.len() - needle.len() {
            let mut found = false;
            for j in 0..needle.len() {
                if self.bios_vec[i + j] != needle[j] {
                    break;
                }
                if j == needle.len() - 1 {
                    found = true;
                }
            }
            if found {
                return i;
            }
        }
        0
    }

    fn bit_entry(&self, id: u8, bit_entry: &mut BitEntry) -> Result {
        let mut entries = self.rd08((self.bit_offset + 0x0a) as isize);
        let rec_size = self.rd08((self.bit_offset + 0x09) as isize);
        let mut entry = (self.bit_offset + 0x0c) as isize;

        while {
            let tmp = entries;
            entries -= 1;
            tmp != 0
        } {
            let idx = self.rd08(entry);
            if idx == id {
                bit_entry.id = self.rd08(entry);
                bit_entry.version = self.rd08(entry + 1);
                bit_entry.length = self.rd16(entry + 2);
                bit_entry.offset = self.rd16(entry + 4);
                return Ok(());
            }
            entry += (rec_size as usize) as isize;
        }
        Err(ENOENT)
    }

    fn pmu_te(&self, ver: &mut u8, hdr: &mut u8, cnt: &mut u8, len: &mut u8) -> u32 {
        let mut bit_p: BitEntry = Default::default();

        let mut data = 0;

        let _ = self.bit_entry(112_u8, &mut bit_p);
        if bit_p.id != 112_u8 {
            return 0;
        }

        if bit_p.version == 2 && bit_p.length >= 4 {
            data = self.rd32(bit_p.offset as isize);
        }
        if data != 0 {
            *ver = self.rd08(data as isize);
            *hdr = self.rd08((data + 0x01) as isize);
            *len = self.rd08((data + 0x02) as isize);
            *cnt = self.rd08((data + 0x03) as isize);
        }
        data
    }

    fn pmu_ee(&self, idx: u8, ver: &mut u8, hdr: &mut u8) -> u32 {
        let mut cnt: u8 = 0;
        let mut len: u8 = 0;
        let mut data = self.pmu_te(ver, hdr, &mut cnt, &mut len);
        if data != 0 && idx < cnt {
            data = data + (*hdr as u32) + (idx as u32 * len as u32);
            *hdr = len;
            return data;
        }
        0
    }

    fn pmu_ep(&self, idx: u8, ver: &mut u8, hdr: &mut u8, info: &mut BiosPmuE) -> u32 {
        let data = self.pmu_ee(idx, ver, hdr);
        if data != 0 {
            info.pmutype = self.rd08(data as isize);
            info.data = self.rd32((data + 0x02) as isize);
        }
        data
    }

    fn pcir_te(&self, offset: isize, ver: &mut u8, hdr: &mut u16) -> u32 {
        let mut data = self.rd16(offset + 0x18) as u32;
        if data != 0 {
            data += offset as u32;
            match self.rd32(data as isize) {
                0x52494350 | 0x53494752 | 0x5344504e => {
                    *hdr = self.rd16((data + 0x0a) as isize);
                    *ver = self.rd08((data + 0x0c) as isize);
                }
                _ => {
                    pr_debug!(
                        "{:#x} Unknown PCIR signature {:#x}\n",
                        data,
                        self.rd32(data as isize)
                    );
                    data = 0;
                    *hdr = 0;
                    *ver = 0;
                }
            }
        }
        data
    }

    fn pcir_tp(&self, offset: isize, ver: &mut u8, hdr: &mut u16, info: &mut BiosPcirT) -> u32 {
        let data = self.pcir_te(offset, ver, hdr);
        if data != 0 {
            info.vendor_id = self.rd16((data + 0x04) as isize);
            info.device_id = self.rd16((data + 0x06) as isize);
            info.class_code[0] = self.rd08((data + 0x0d) as isize);
            info.class_code[1] = self.rd08((data + 0x0e) as isize);
            info.class_code[2] = self.rd08((data + 0x0f) as isize);
            info.image_size = (self.rd16((data + 0x10) as isize) as u32) * 512;
            info.image_type = self.rd08((data + 0x14) as isize);
            info.last = self.rd08((data + 0x15) as isize) != 0;
        }
        data
    }

    fn npde_te(&self, offset: isize) -> u32 {
        let mut pcir: BiosPcirT = Default::default();
        let mut ver: u8 = 0;
        let mut hdr: u16 = 0;
        let mut data = self.pcir_tp(offset, &mut ver, &mut hdr, &mut pcir);
        data = (data + (hdr as u32) + 0x0f) & !0x0f;
        if data != 0 {
            match self.rd32(data as isize) {
                0x4544504e => {}
                _ => {
                    pr_debug!(
                        "{:#x} Unknown NPDE signature {:#x}\n",
                        data,
                        self.rd32(data as isize)
                    );
                    data = 0;
                }
            }
        }
        data
    }

    fn npde_tp(&self, offset: isize, info: &mut BiosNpdeT) -> u32 {
        let data = self.npde_te(offset);
        if data != 0 {
            info.image_size = (self.rd16((data + 0x08) as isize) as u32) * 512;
            info.last = (self.rd08((data + 0x0a) as isize) & 0x80) != 0;
        }
        data
    }

    fn imagen(&self, image: &mut BiosImage) -> bool {
        let data = self.rd16(image.base as isize);

        pr_info!("bios {:#x}", data);

        match data {
            0xaa55 | 0xbb77 | 0x4e56 => {}
            _ => return false,
        };

        let mut pcir: BiosPcirT = Default::default();
        let mut ver: u8 = 0;
        let mut hdr: u16 = 0;

        let data = self.pcir_tp(image.base as isize, &mut ver, &mut hdr, &mut pcir);
        if data == 0 {
            return false;
        }
        image.size = pcir.image_size as usize;
        image.itype = pcir.image_type;
        image.last = pcir.last;

        if pcir.image_type != 0x70 {
            let mut npde: BiosNpdeT = Default::default();
            let data = self.npde_tp(image.base as isize, &mut npde);
            if data != 0 {
                image.size = npde.image_size as usize;
                image.last = npde.last;
            }
        } else {
            image.last = true;
        }
        true
    }

    fn fetch(
        &mut self,
        bar: &RevocableGuard<'_, pci::Bar>,
        offset: usize,
        length: usize,
    ) -> Result {
        let vec = &mut self.bios_vec;
        let gfp = self.bios_vec_gfp;

        vec.resize(offset + length, 0, gfp)?;
        for i in (offset..offset + length).step_by(4) {
            let ptr: *mut u32 = vec.as_mut_ptr() as *mut u32;
            unsafe {
                *(ptr.add(i / 4)) = bar.readl(PROM_OFFSET + i)?;
            }
        }
        Ok(())
    }

    pub(crate) fn probe(&mut self, bar: &Devres<pci::Bar>) -> Result {
        /* just do PROM probe */
        /* hardcoded lots */
        let bar = bar.try_access().ok_or(ENXIO)?;
        let mut data = bar.readl(0x88000 + 0x50)?;
        data &= !0x00000001;
        bar.writel(data, 0x88000 + 0x50)?;

        let mut image: BiosImage = Default::default();
        let mut idx = 0;
        loop {
            self.fetch(&bar, image.base, image.base + 4096)?;

            let imaged_addr = self.imaged_addr;
            self.imaged_addr = 0;
            if !self.imagen(&mut image) {
                self.imaged_addr = imaged_addr;
                break;
            }

            if idx == 0 {
                self.image0_size = image.size as isize;
            }

            pr_info!(
                "bios pcir: {:#x} {:#x} {}\n",
                image.size,
                image.itype,
                image.last
            );

            if image.size > 0x100000 {
                break;
            }

            if image.itype == 0xe0 {
                self.imaged_addr = image.base;
            }

            self.fetch(&bar, image.base, image.size)?;

            if image.last {
                self.imaged_addr = imaged_addr;
                break;
            }

            image.base += image.size;
            if image.base > 0x100000 {
                break;
            }
            idx += 1;
        }

        pr_info!("bios size {:#x}\n", self.bios_vec.len());
        let mut bmp_vec = Vec::new(); //vec![];//0xff, 0x7f, 0x78, 0x86, 0x00];
        bmp_vec.push(0xff, GFP_KERNEL)?;
        bmp_vec.push(0x7f, GFP_KERNEL)?;
        bmp_vec.push(0x78, GFP_KERNEL)?;
        bmp_vec.push(0x86, GFP_KERNEL)?;
        bmp_vec.push(0x00, GFP_KERNEL)?;
        self.bmp_offset = self.findbytes(&bmp_vec);
        pr_info!(
            "bmp offset is {:#x} {:#x}\n",
            bmp_vec.len(),
            self.bmp_offset
        );

        let mut bit_vec = Vec::new(); //ec![0xff, 0xb8, 'B', 'I', 'T'];
        bit_vec.push(0xff, GFP_KERNEL)?;
        bit_vec.push(0xb8, GFP_KERNEL)?;
        bit_vec.push(b'B', GFP_KERNEL)?;
        bit_vec.push(b'I', GFP_KERNEL)?;
        bit_vec.push(b'T', GFP_KERNEL)?;
        self.bit_offset = self.findbytes(&bit_vec);
        pr_info!(
            "bit offset is {:#x} {:#x}\n",
            bit_vec.len(),
            self.bit_offset
        );

        let mut bit_entry: BitEntry = Default::default();

        self.bit_entry(b'i', &mut bit_entry)?;
        pr_info!("bit i {:#x} {:#x}\n", bit_entry.offset, bit_entry.length);

        if bit_entry.length >= 4 {
            let major = self.rd08((bit_entry.offset + 3) as isize);
            let chip = self.rd08((bit_entry.offset + 2) as isize);
            let minor = self.rd08((bit_entry.offset + 1) as isize);
            #[allow(clippy::identity_op)]
            let micro = self.rd08((bit_entry.offset + 0) as isize);
            let patch = self.rd08((bit_entry.offset + 4) as isize);

            pr_info!(
                "version {:x}:{:x}:{:x}:{:x}:{:x}\n",
                major,
                chip,
                minor,
                micro,
                patch
            );
        }

        Ok(())
    }

    pub(crate) fn find_fwsec(&mut self) -> Result {
        // find fwsec?
        let mut idx = 0;
        let mut ver = 0;
        let mut hdr = 0;
        let mut pmue: BiosPmuE = Default::default();
        let mut found: i8 = -1;
        loop {
            let data = self.pmu_ep(idx, &mut ver, &mut hdr, &mut pmue);
            if data == 0 {
                break;
            }
            if pmue.pmutype == 0x85 {
                found = idx as i8;
                break;
            }
            idx += 1;
        }

        match found {
            -1 => {
                return Err(ENOENT);
            }
            _ => {
                pr_info!("pmu found idx {}\n", found);
            }
        }

        let desc_hdr = self.rd32(pmue.data as isize);
        pr_info!(
            "flcn {:#x} {} {}\n",
            desc_hdr,
            (desc_hdr & 0xffff0000) >> 16,
            (desc_hdr & 0xff00) >> 8
        );

        let ucode_ptr = self.ptr(pmue.data as isize);
        unsafe {
            let ucodedesc: *const FalconUCodeDescV3 = ucode_ptr as *const FalconUCodeDescV3;
            pr_info!(
                "ucode desc stored {:#x}, imem {:#x} {:#x} {:#x} dmem {:#x} {:#x}\n",
                (*ucodedesc).StoredSize,
                (*ucodedesc).IMEMPhysBase,
                (*ucodedesc).IMEMLoadSize,
                (*ucodedesc).IMEMVirtBase,
                (*ucodedesc).DMEMPhysBase,
                (*ucodedesc).DMEMLoadSize
            );
        }
        Ok(())
    }
}
