fn run_functional() {
    let mut dut = Arith::new();
    dut.a = Wire::<19>::new(3);
    dut.b = Wire::<19>::new(4);
    dut.eval();
    assert_eq!(dut.sum.value(), 7, "arith sum mismatch");
}

fn run_perf(cycles: u64) {
    let mut dut = Arith::new();
    dut.a = Wire::<19>::new(1);
    dut.b = Wire::<19>::new(2);
    let start = std::time::Instant::now();
    for i in 0..cycles {
        dut.a = Wire::<19>::new(i);
        dut.b = Wire::<19>::new(i.wrapping_mul(3));
        dut.eval();
        std::hint::black_box(dut.sum.value());
    }
    let secs = start.elapsed().as_secs_f64();
    let hz = if secs > 0.0 { (cycles as f64) / secs } else { 0.0 };
    println!(
        "{{\"backend\":\"rust\",\"design\":\"arith\",\"cycles\":{cycles},\"seconds\":{secs:.6},\"hz\":{hz:.1}}}"
    );
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.get(1).map(String::as_str) == Some("perf") {
        let cycles = args.get(2).and_then(|s| s.parse().ok()).unwrap_or(1_000_000);
        run_perf(cycles);
    } else {
        run_functional();
        println!("ok");
    }
}
