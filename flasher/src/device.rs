//! HID transport for the RY5088 flasher: enumerate/open, GET_INFOR, enter-bootloader, and the flash stream.
//! All the byte-level protocol lives in `proto`; this module is the I/O + sequencing around it.

use crate::proto;
use hidapi::{HidApi, HidDevice};
use std::thread::sleep;
use std::time::{Duration, Instant};

pub const VID: u16 = 0x3151;
pub const PID_NORMAL: u16 = 0x5030;
/// Normal-mode PIDs this flasher will open. 0x5030 is the most common, but devices.json puts many RY5088
/// boards elsewhere (e.g. FUN60 Ultra 2381 on 0x502D). The PID only gates which handle we open; the
/// dev_id from GET_INFOR is still the model discriminator.
pub const PIDS_NORMAL: &[u16] = &[0x5030, 0x502D];
pub const PID_BOOT: u16 = 0x502A;
const UP_NORMAL: u16 = 0xFFFF;
const UP_BOOT: u16 = 0xFF01;

pub struct Dev {
    api: HidApi,
}

fn ms(n: u64) -> Duration { Duration::from_millis(n) }

/// send_feature_report wants [report_id=0, ...64 payload bytes].
fn send(d: &HidDevice, frame: &[u8; 64]) -> Result<(), String> {
    let mut buf = [0u8; 65];
    buf[1..].copy_from_slice(frame);
    d.send_feature_report(&buf).map_err(|e| e.to_string())
}

/// Read a feature report; normalise away a possible leading report-id 0 so the first byte is the opcode echo.
fn recv(d: &HidDevice) -> Option<Vec<u8>> {
    let mut buf = [0u8; 65];
    buf[0] = 0;
    let n = d.get_feature_report(&mut buf).ok()?;
    if n == 0 { return None; }
    Some(buf[..n].to_vec())
}

impl Dev {
    pub fn new() -> Result<Self, String> {
        HidApi::new().map(|api| Dev { api }).map_err(|e| e.to_string())
    }

    pub fn refresh(&mut self) {
        let _ = self.api.refresh_devices();
    }

    fn open(&self, pids: &[u16], up: u16, usage: Option<u16>) -> Option<HidDevice> {
        for info in self.api.device_list() {
            if info.vendor_id() == VID
                && pids.contains(&info.product_id())
                && info.usage_page() == up
                && usage.map_or(true, |u| info.usage() == u)
            {
                if let Ok(d) = info.open_device(&self.api) {
                    return Some(d);
                }
            }
        }
        None
    }

    fn present(&self, pids: &[u16], up: u16) -> bool {
        self.api.device_list().any(|i| i.vendor_id() == VID && pids.contains(&i.product_id()) && i.usage_page() == up)
    }

    pub fn normal_present(&self) -> bool { self.present(PIDS_NORMAL, UP_NORMAL) }
    pub fn boot_present(&self) -> bool { self.present(&[PID_BOOT], UP_BOOT) }

    /// Read GET_INFOR from the connected keyboard (normal mode). Returns (dev_id, version).
    pub fn read_infor(&self) -> Option<(u16, String)> {
        let d = self.open(PIDS_NORMAL, UP_NORMAL, Some(2))?;
        send(&d, &proto::get_infor()).ok()?;
        sleep(ms(50));
        let r = recv(&d)?;
        // tolerate a leading report-id 0 (binding/platform dependent)
        proto::parse_infor(&r).or_else(|| if r.len() > 1 { proto::parse_infor(&r[1..]) } else { None })
    }

    /// Open the normal-mode device and read its USB manufacturer / product / serial strings.
    pub fn usb_strings(&self) -> (Option<String>, Option<String>, Option<String>) {
        match self.open(PIDS_NORMAL, UP_NORMAL, Some(2)) {
            Some(d) => (
                d.get_manufacturer_string().ok().flatten(),
                d.get_product_string().ok().flatten(),
                d.get_serial_number_string().ok().flatten(),
            ),
            None => (None, None, None),
        }
    }

    /// Send the enter-bootloader sequence (WIPES config) and wait up to ~20s for re-enumeration to 502A.
    pub fn enter_bootloader(&mut self) -> Result<(), String> {
        let d = self.open(PIDS_NORMAL, UP_NORMAL, Some(2)).ok_or("not in normal mode (3151:5030/502D)")?;
        send(&d, &proto::isp_prepare()).map_err(|e| format!("enter-bootloader (ISP_PREPARE) failed: {e}"))?;
        sleep(ms(100));
        // This command resets the device, so its transfer may report an error as the port drops — that is
        // expected; the authoritative signal is re-enumeration to the bootloader, polled below.
        let _ = send(&d, &proto::enter_bootloader());
        drop(d);
        let t0 = Instant::now();
        while t0.elapsed() < Duration::from_secs(20) {
            sleep(ms(500));
            self.refresh();
            if self.boot_present() {
                return Ok(());
            }
        }
        Err("did not re-enumerate to bootloader (502A) within 20s".into())
    }

    /// Flash a 0x5000-slice while in bootloader mode. `progress(done, total)` per chunk. Returns the ACK
    /// (None if the device reset before ACK — treat success via re-enumeration, see `wait_normal`).
    pub fn flash_slice<F: FnMut(usize, usize)>(&self, slice: &[u8], mut progress: F) -> Result<Option<bool>, String> {
        let d = self.open(&[PID_BOOT], UP_BOOT, None).ok_or("not in bootloader mode (3151:502A)")?;
        let chunks = proto::chunkify(slice);
        if chunks.len() > u16::MAX as usize {
            return Err(format!("image too large: {} chunks exceeds the 16-bit transfer limit", chunks.len()));
        }
        let cc = chunks.len() as u16;
        let cks = proto::fw_checksum(slice);
        let size = slice.len() as u32;

        send(&d, &proto::build_start(cc, size))?;
        sleep(ms(30));
        for (i, ch) in chunks.iter().enumerate() {
            send(&d, ch)?;
            progress(i + 1, chunks.len());
            // The bootloader handles one report per main-loop pass (copy, program, read-back); a report that
            // lands before the previous one is consumed overwrites it and that chunk is silently dropped.
            // 2 ms outran it on Linux hidraw (FUN60 Ultra 2381); 10 ms verified.
            sleep(ms(10));
        }
        sleep(ms(30));
        send(&d, &proto::build_complete(cc, cks))?;
        sleep(ms(60));
        // best-effort ACK: a successful flash resets the device immediately, so this may legitimately fail.
        match recv(&d) {
            Some(a) => {
                let ok = proto::ack_ok(&a) || (a.len() > 1 && proto::ack_ok(&a[1..]));
                let fail = (a.len() > 4 && a[0] == 0xAB && a[4] == 0xAA)
                    || (a.len() > 5 && a[1] == 0xAB && a[5] == 0xAA);
                Ok(if ok { Some(true) } else if fail { Some(false) } else { None })
            }
            None => Ok(None), // device likely reset -> verify by re-enumeration
        }
    }

    /// Wait up to ~20s for return to normal mode (the authoritative success signal after a flash).
    pub fn wait_normal(&mut self) -> bool {
        let t0 = Instant::now();
        while t0.elapsed() < Duration::from_secs(20) {
            sleep(ms(500));
            self.refresh();
            if self.normal_present() {
                return true;
            }
        }
        false
    }
}
