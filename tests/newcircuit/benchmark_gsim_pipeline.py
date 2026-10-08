"""Reproducible synthetic activity benchmark; not a GSIM workload comparison."""
import argparse
import json
import os
from pathlib import Path
import statistics
import subprocess
import tempfile
import time

repo = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--iterations', type=int, default=1_000_000)
args = parser.parse_args()
pycc = os.environ.get('PYCC', str(repo / '.pycircuit_out/toolchain/build/bin/pycc'))
cxx = os.environ.get('CXX', 'c++')
results = {'workload': '260-stage scalar add DAG, state changes every 64 evaluations',
           'iterations': args.iterations, 'repeats': 3, 'variants': {}}
harness = r'''
#include "model.cpp"
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
int main(int argc, char **argv) {
  const unsigned iterations = std::strtoul(argv[1], nullptr, 10);
  pyc::gen::top sim;
  sim.en = pyc::cpp::Wire<1>(1);
  std::uint32_t random = 0x1637abcdu;
  std::uint64_t checksum = 0, expectedChecksum = 0;
  unsigned q = 0;
  const auto start = std::chrono::steady_clock::now();
  for (unsigned i = 0; i < iterations; ++i) {
    if ((i & 63u) == 0) {
      sim.clk = pyc::cpp::Wire<1>(0);
      sim.tick_compute(); sim.tick_commit();
      random ^= random << 13; random ^= random >> 17; random ^= random << 5;
      q = random & 65535u;
      sim.d = pyc::cpp::Wire<16>(q);
      sim.clk = pyc::cpp::Wire<1>(1);
      sim.tick_compute(); sim.tick_commit();
    }
    sim.eval();
    const unsigned lane = i % 260;
    checksum += sim.out[lane].value();
    expectedChecksum += (q * (lane + 2)) & 65535u;
  }
  const double seconds = std::chrono::duration<double>(
      std::chrono::steady_clock::now() - start).count();
  if (checksum != expectedChecksum) return 1;
  std::cout << "{\"seconds\":" << seconds
            << ",\"checksum\":" << checksum << "}\n";
}
'''
with tempfile.TemporaryDirectory(prefix='newcircuit-benchmark-') as temp:
    work = Path(temp)
    subprocess.run(['python3', str(repo / 'tests/newcircuit/generate_activity_fanout.py'),
                    str(work / 'design.mlir'), '260'], check=True)
    (work / 'harness.cpp').write_text(harness)
    for mode in ('default', 'activation_off'):
        command = [pycc, str(work / 'design.mlir'), '--emit=cpp', '--logic-depth=512',
                   '-o', str(work / 'model.cpp')]
        if mode == 'activation_off':
            command += ['--sim-group-activation=false']
        start = time.perf_counter()
        subprocess.run(command, check=True)
        generation = time.perf_counter() - start
        start = time.perf_counter()
        subprocess.run([cxx, '-std=c++17', '-O2', '-I', str(repo / 'runtime'),
                        str(work / 'harness.cpp'), '-o', str(work / 'sim')], check=True)
        compilation = time.perf_counter() - start
        samples = []
        for _ in range(3):
            output = subprocess.check_output([str(work / 'sim'), str(args.iterations)],
                                             text=True)
            samples.append(json.loads(output))
        elapsed = statistics.median(sample['seconds'] for sample in samples)
        results['variants'][mode] = {
            'generation_seconds': generation, 'cpp_compile_seconds': compilation,
            'cpp_bytes': (work / 'model.cpp').stat().st_size,
            'median_simulation_seconds': elapsed,
            'evaluations_per_second': args.iterations / elapsed,
            'samples': samples,
        }
assert len({sample['checksum'] for variant in results['variants'].values()
            for sample in variant['samples']}) == 1
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(json.dumps(results, indent=2) + '\n')
print(json.dumps(results, indent=2))
