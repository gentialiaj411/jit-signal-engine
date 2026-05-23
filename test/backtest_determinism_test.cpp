#include <fstream>
#include <iostream>
#include <string>

int main() {
#if defined(_WIN32)
  std::puts("SKIP: backtest determinism test is WSL/Linux-only");
  return 0;
#else
  const std::string runner = "./backtest_runner";
  const std::string journal = "/mnt/c/Users/bhask/Documents/PROJECTS/market-data-handler/bench/results/itch_1m_ab_source.journal";
  const std::string signal = "../examples/filtered_momentum.sig";
  const std::string out_root = "../bench/results/backtest";

  const std::string cmd_a = runner + " --journal " + journal + " --signal-file " + signal +
      " --out-root " + out_root + " --run-id determinism_a --max-events 200000 --seed 42";
  const std::string cmd_b = runner + " --journal " + journal + " --signal-file " + signal +
      " --out-root " + out_root + " --run-id determinism_b --max-events 200000 --seed 42";

  if (std::system(cmd_a.c_str()) != 0 || std::system(cmd_b.c_str()) != 0) {
    std::cerr << "backtest_runner execution failed\n";
    return 1;
  }

  std::ifstream a("../bench/results/backtest/determinism_a/ic_report.json");
  std::ifstream b("../bench/results/backtest/determinism_b/ic_report.json");
  if (!a || !b) {
    std::cerr << "missing ic report(s)\n";
    return 1;
  }
  const std::string sa((std::istreambuf_iterator<char>(a)), std::istreambuf_iterator<char>());
  const std::string sb((std::istreambuf_iterator<char>(b)), std::istreambuf_iterator<char>());

  auto extract = [](const std::string& s) {
    auto h1 = s.find("\"per_signal_ic_at_horizon_1\"");
    auto h5 = s.find("\"per_signal_ic_at_horizon_5\"");
    auto h30 = s.find("\"per_signal_ic_at_horizon_30\"");
    if (h1 == std::string::npos || h5 == std::string::npos || h30 == std::string::npos) return std::string{};
    return s.substr(h1, h30 - h1 + 64);
  };

  if (extract(sa) != extract(sb)) {
    std::cerr << "IC sections differ across identical reruns\n";
    return 1;
  }

  std::puts("backtest_determinism_test: PASS");
  return 0;
#endif
}
