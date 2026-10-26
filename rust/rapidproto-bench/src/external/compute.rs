//! `compute Instance`: the C++ arena bench's large-schema scenario (`tests/bench_arm_compute.cpp`) --
//! one googleapis compute.proto `Instance`, the payload its dump writes. Arms: cold (the baseline) and
//! warm, as in C++ (which has no streaming decoder for compute.proto).

#[allow(
    clippy::all,
    unused,
    non_camel_case_types,
    non_snake_case,
    non_upper_case_globals
)]
mod generated {
    include!(concat!(env!("OUT_DIR"), "/compute.rs"));
}

use generated::arena::google::cloud::compute::v1beta::Instance;
use rapidproto::ProtoStr;

use crate::Arm;

pub fn arms() -> Vec<Arm> {
    arena_arms!(Instance, checksum)
}

fn len(v: Option<&ProtoStr>) -> u64 {
    v.map_or(0, |v| v.len() as u64)
}

// The C++ `checksum_compute`: every repeated/map field the payload populates is touched, so the sum is
// a function of the whole decoded tree.
fn checksum(inst: &Instance) -> u64 {
    let mut s = len(inst.name()).wrapping_add(len(inst.description()));
    let mut add = |v: u64| s = s.wrapping_add(v);
    for disk in inst.disks() {
        add(len(disk.device_name()));
        add(disk.disk_size_gb().unwrap_or(0) as u64);
        add(len(disk.r#type()));
    }
    for nic in inst.network_interfaces() {
        add(len(nic.name()).wrapping_add(len(nic.network())));
    }
    for (k, v) in inst.labels().iter() {
        add((k.len() + v.len()) as u64);
    }
    for acct in inst.service_accounts() {
        add(len(acct.email()).wrapping_add(acct.scopes().len() as u64));
    }
    for acc in inst.guest_accelerators() {
        add(i64::from(acc.accelerator_count().unwrap_or(0)) as u64);
    }
    if let Some(meta) = inst.metadata() {
        for item in meta.items() {
            add(len(item.key()).wrapping_add(len(item.value())));
        }
    }
    if let Some(sched) = inst.scheduling() {
        add(u64::from(sched.automatic_restart().unwrap_or(false)));
    }
    s
}
