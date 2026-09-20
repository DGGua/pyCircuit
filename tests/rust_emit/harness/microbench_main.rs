fn full_cycle(dut: &mut Microbench) {
    dut.clk = true;
    dut.eval();
    dut.tick();
    dut.transfer();
    dut.eval();
    dut.clk = false;
    dut.eval();
    dut.tick();
    dut.transfer();
    dut.eval();
}

fn run_functional() {
    let mut dut = Microbench::new();
    dut.rst = true;
    dut.clk = false;
    dut.sel = true;
    dut.addend = 1;
    dut.eval();
    for _ in 0..2 {
        full_cycle(&mut dut);
    }
    dut.rst = false;
    for _ in 0..8 {
        full_cycle(&mut dut);
    }
    println!("acc_out={}", dut.acc_out);
}

fn run_perf(cycles: u64) {
    let mut dut = Microbench::new();
    dut.rst = false;
    dut.sel = true;
    dut.addend = 1;
    dut.clk = false;
    dut.eval();
    let start = std::time::Instant::now();
    for _ in 0..cycles {
        full_cycle(&mut dut);
        std::hint::black_box(dut.acc_out);
    }
    let secs = start.elapsed().as_secs_f64();
    let hz = if secs > 0.0 { (cycles as f64) / secs } else { 0.0 };
    println!(
        "{{\"backend\":\"rust\",\"design\":\"microbench\",\"cycles\":{cycles},\"seconds\":{secs:.6},\"hz\":{hz:.1}}}"
    );
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.get(1).map(String::as_str) == Some("perf") {
        let cycles = args.get(2).and_then(|s| s.parse().ok()).unwrap_or(50_000);
        run_perf(cycles);
    } else {
        run_functional();
        println!("ok");
    }
}
