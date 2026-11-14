#![no_main]
use libfuzzer_sys::fuzz_target;

fuzz_target!(|data: &[u8]| {
    aether_asset_loader::fuzz_entry::fuzz_vrm_json(data);
});
