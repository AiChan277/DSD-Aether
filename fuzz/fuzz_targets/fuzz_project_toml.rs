#![no_main]
use libfuzzer_sys::fuzz_target;

fuzz_target!(|data: &[u8]| {
    aether_plugin_host::fuzz_entry::fuzz_project_toml(data);
});
