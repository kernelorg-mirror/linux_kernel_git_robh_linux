#![allow(non_snake_case)]
#[allow(dead_code)]
#[repr(C)]
pub(crate) struct FalconUCodeDescV2 {
    Hdr: u32,
    StoredSize: u32,
    UncompressedSize: u32,
    VirtualEntry: u32,
    InterfaceOffset: u32,
    IMEMPhysBase: u32,
    IMEMLoadSize: u32,
    IMEMVirtBase: u32,
    IMEMSecBase: u32,
    IMEMSecSize: u32,
    DMEMOffset: u32,
    DMEMLoadSize: u32,
    altIMEMLoadSize: u32,
    altDMEMLoadSize: u32,
}

#[repr(C)]
pub(crate) struct FalconUCodeDescV3 {
    pub(crate) Hdr: u32,
    pub(crate) StoredSize: u32,
    PKCDataOffset: u32,
    InterfaceOffset: u32,
    pub(crate) IMEMPhysBase: u32,
    pub(crate) IMEMLoadSize: u32,
    pub(crate) IMEMVirtBase: u32,
    pub(crate) DMEMPhysBase: u32,
    pub(crate) DMEMLoadSize: u32,
    EngineIdMask: u16,
    UcodeId: u8,
    SignatureCount: u8,
    SignatureVersions: u16,
    Reserved: u16,
}
