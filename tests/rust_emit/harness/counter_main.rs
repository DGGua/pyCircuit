fn full_cycle(dut: &mut Counter) {
    dut.clk = Wire::<1>::new(1);
    dut.eval();
    dut.tick();
    dut.transfer();
    dut.eval();
    dut.clk = Wire::<1>::new(0);
    dut.eval();
    dut.tick();
    dut.transfer();
    dut.eval();
}

fn run_functional() {
    let mut dut = Counter::new();
    dut.enable = Wire::<1>::new(0);
    dut.rst = Wire::<1>::new(1);
    dut.clk = Wire::<1>::new(0);
    dut.eval();
    for _ in 0..2 {
        full_cycle(&mut dut);
    }
    dut.rst = Wire::<1>::new(0);
    full_cycle(&mut dut);
    dut.enable = Wire::<1>::new(1);
    for expect in 1u64..=5 {
        full_cycle(&mut dut);
        let got = dut.count.value();
        println!("count={got}");
        assert_eq!(got, expect, "counter functional mismatch");
    }
}

fn run_perf(cycles: u64) {
    let mut dut = Counter::new();
    dut.enable = Wire::<1>::new(1);
    dut.rst = Wire::<1>::new(0);
    dut.clk = Wire::<1>::new(0);
    dut.eval();
    let start = std::time::Instant::now();
    for _ in 0..cycles {
        full_cycle(&mut dut);
        std::hint::black_box(dut.count.value());
    }
    let secs = start.elapsed().as_secs_f64();
    let hz = if secs > 0.0 { (cycles as f64) / secs } else { 0.0 };
    println!(
        "{{\"backend\":\"rust\",\"design\":\"counter\",\"cycles\":{cycles},\"seconds\":{secs:.6},\"hz\":{hz:.1}}}"
    );
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.get(1).map(String::as_str) == Some("perf") {
        let cycles = args.get(2).and_then(|s| s.parse().ok()).unwrap_or(200_000);
        run_perf(cycles);
    } else {
        run_functional();
        println!("ok");
    }
}
