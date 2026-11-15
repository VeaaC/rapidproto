// TEMPORARY: runs hello.wasm (a Rust wasm32-wasip1 program that reads a line from stdin and prints
// it) in wasmi with in-memory stdio, as rapidproto-build does.
use wasmi::{Engine, Linker, Module, Store};
use wasmi_wasi::sync::WasiCtxBuilder;
use wasmi_wasi::wasi_common::pipe::{ReadPipe, WritePipe};
use wasmi_wasi::WasiCtx;

fn main() {
    let engine = Engine::default();
    let module = Module::new(&engine, &include_bytes!("../hello.wasm")[..]).unwrap();
    let stdout = WritePipe::new_in_memory();
    let wasi = WasiCtxBuilder::new()
        .stdin(Box::new(ReadPipe::from("ping\n")))
        .stdout(Box::new(stdout.clone()))
        .build();
    let mut store = Store::new(&engine, wasi);
    let mut linker: Linker<WasiCtx> = Linker::new(&engine);
    wasmi_wasi::sync::add_to_linker(&mut linker, |ctx| ctx).unwrap();
    eprintln!("host: instantiating");
    let instance = linker.instantiate_and_start(&mut store, &module).unwrap();
    let start = instance.get_typed_func::<(), ()>(&store, "_start").unwrap();
    eprintln!("host: calling _start");
    start.call(&mut store, ()).unwrap();
    drop(store);
    let out = stdout.try_into_inner().unwrap().into_inner();
    println!("host: guest wrote {:?}", String::from_utf8_lossy(&out));
}
