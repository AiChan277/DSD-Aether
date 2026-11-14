#![deny(clippy::pedantic)]
#![forbid(unsafe_code)]

//! Deterministic corpus + mutation fuzz smoke runner for Windows (`fuzz_project_toml`, `fuzz_graph_json`, `fuzz_osc`).

use std::time::Instant;

use aether_plugin_host::fuzz_entry::{fuzz_graph_json, fuzz_osc, fuzz_project_toml};

fn next_u64(state: &mut u64) -> u64 {
    let mut x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    x
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let iterations: usize = args
        .get(1)
        .and_then(|s| s.parse::<usize>().ok())
        .unwrap_or(50_000);

    let seeds_toml: [&[u8]; 3] = [
        b"schema_version = 1\napp_version = \"0.9.0\"\navatar_path = \"v1.vrm\"\n[render]\nz_flatten = 0.5\n",
        b"schema_version = 2\n[avatar]\npath = \"x.vrm\"\n[unknown_section]\nfoo = 123\n",
        b"",
    ];
    let seeds_graph: [&[u8]; 3] = [
        br#"{"schema_version":1,"nodes":[{"id":1,"type":"aether.source.npu_pose@1"},{"id":2,"type":"aether.output.pose@1"}],"edges":[{"id":1,"from_node":1,"from_port":"pose","to_node":2,"to_port":"pose"}]}"#,
        br#"{"schema_version":1,"nodes":[{"id":1,"type":"aether.math.delay@1"},{"id":2,"type":"aether.math.add@1"}],"edges":[{"from_node":1,"from_port":"out","to_node":2,"to_port":"a"},{"from_node":2,"from_port":"out","to_node":1,"to_port":"in"}]}"#,
        b"{bad",
    ];
    let seeds_osc: [&[u8]; 3] = [
        b"/aether/param/render.flatness\0\0\0,f\0\0?\0\0\0",
        b"/aether/hotkey/1\0\0\0\0,i\0\0\0\0\0\x01",
        b"#bundle\0\0\0\0\0\0\0\0\x01\0\0\0\x14/aether/expression/joy\0\0,f\0\0?\x80\0\0",
    ];

    let start = Instant::now();
    let mut rng = 0x5EED_CAFE_2026_u64;
    let mut scratch = Vec::with_capacity(4096);

    for i in 0..iterations {
        let base = match i % 3 {
            0 => seeds_toml[(i / 3) % seeds_toml.len()],
            1 => seeds_graph[(i / 3) % seeds_graph.len()],
            _ => seeds_osc[(i / 3) % seeds_osc.len()],
        };
        scratch.clear();
        scratch.extend_from_slice(base);
        let muts = usize::try_from(next_u64(&mut rng) % 8).unwrap_or(1);
        for _ in 0..muts {
            let op = next_u64(&mut rng) % 4;
            match op {
                0 if !scratch.is_empty() => {
                    let pos =
                        usize::try_from(next_u64(&mut rng) % (scratch.len() as u64)).unwrap_or(0);
                    scratch[pos] ^= (next_u64(&mut rng) & 0xFF) as u8;
                }
                1 if scratch.len() < 4096 => {
                    scratch.push((next_u64(&mut rng) & 0xFF) as u8);
                }
                2 if !scratch.is_empty() => {
                    let cut =
                        usize::try_from(next_u64(&mut rng) % (scratch.len() as u64)).unwrap_or(0);
                    scratch.truncate(cut);
                }
                _ => {}
            }
        }
        match i % 3 {
            0 => fuzz_project_toml(&scratch),
            1 => fuzz_graph_json(&scratch),
            _ => fuzz_osc(&scratch),
        }
    }

    println!(
        "plugin_fuzz_runner completed {} iterations across toml/graph/osc in {:.2} s (0 crashes).",
        iterations,
        start.elapsed().as_secs_f64()
    );
}
