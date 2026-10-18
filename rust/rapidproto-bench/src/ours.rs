//! Arm 0 / the baseline: RapidProto's generated arena decoder (reused from the golden crate, so the
//! comparison runs the exact code a consumer compiles). Decodes into a fresh arena and walks every
//! field with the shared [`checksum`](crate::checksum).

use rapidproto::Arena;
use rustgen_golden::bench::arena::bench::{BigSet, Dataset, ParticleSet, RecordSet, WideSet};

use crate::{checksum, Arm};

pub fn dataset() -> Arm {
    Arm {
        name: "rapidproto",
        run: Box::new(|b| {
            let arena = Arena::new();
            checksum::checksum_dataset(Dataset::decode(b, &arena).expect("rapidproto decode"))
        }),
    }
}

pub fn bigset() -> Arm {
    Arm {
        name: "rapidproto",
        run: Box::new(|b| {
            let arena = Arena::new();
            checksum::checksum_bigset(BigSet::decode(b, &arena).expect("rapidproto decode"))
        }),
    }
}

pub fn wideset() -> Arm {
    Arm {
        name: "rapidproto",
        run: Box::new(|b| {
            let arena = Arena::new();
            checksum::checksum_wideset(WideSet::decode(b, &arena).expect("rapidproto decode"))
        }),
    }
}

pub fn recordset() -> Arm {
    Arm {
        name: "rapidproto",
        run: Box::new(|b| {
            let arena = Arena::new();
            checksum::checksum_recordset(RecordSet::decode(b, &arena).expect("rapidproto decode"))
        }),
    }
}

pub fn particleset() -> Arm {
    Arm {
        name: "rapidproto",
        run: Box::new(|b| {
            let arena = Arena::new();
            checksum::checksum_particleset(
                ParticleSet::decode(b, &arena).expect("rapidproto decode"),
            )
        }),
    }
}
