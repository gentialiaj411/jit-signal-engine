#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ast_utils.h"
#include "jit_compiler.h"
#include "runtime.h"
#include "signal_program.h"

namespace fs = std::filesystem;

namespace {

struct Options {
  std::string signal_file;
  std::string ticks_csv;
  std::string output_signal;
  std::string out_dir;
  std::string artifact_stem{"calibration_fixture_fit"};
  std::string target_column{"target"};
  std::string input_symbol{"AAPL"};
  double learning_rate{0.05};
  double beta1{0.9};
  double beta2{0.999};
  double epsilon{1e-8};
  std::size_t iterations{80};
  double min_improvement{0.0};
};

struct TickRow {
  std::uint64_t timestamp_ns = 0;
  double bid = 0.0;
  double ask = 0.0;
  double volume = 100.0;
  double target = 0.0;
};

struct PreparedProgram {
  jitse::ProgramDef program;
  jitse::SymbolTable symbols;
  std::size_t output_index = 0;
  std::uint32_t input_symbol_id = 0;
};

struct EvalResult {
  double objective = 0.0;
  std::vector<double> gradient;
};

std::string ReadFile(const std::string& path) {
  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error("Failed to open file: " + path);
  }
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::vector<std::string> SplitCsvLine(const std::string& line) {
  std::vector<std::string> fields;
  std::string field;
  std::istringstream in(line);
  while (std::getline(in, field, ',')) {
    while (!field.empty() && (field.back() == '\r' || field.back() == '\n')) {
      field.pop_back();
    }
    fields.push_back(field);
  }
  return fields;
}

std::string JoinCommand(int argc, char** argv) {
  std::ostringstream out;
  for (int i = 0; i < argc; ++i) {
    if (i != 0) out << ' ';
    out << argv[i];
  }
  return out.str();
}

std::string JsonEscape(const std::string& s) {
  std::ostringstream out;
  for (char c : s) {
    switch (c) {
      case '\\': out << "\\\\"; break;
      case '"': out << "\\\""; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default: out << c; break;
    }
  }
  return out.str();
}

std::string ExecRead(const char* cmd) {
  std::array<char, 256> buf{};
#ifdef _WIN32
  FILE* pipe = _popen(cmd, "r");
#else
  FILE* pipe = popen(cmd, "r");
#endif
  if (pipe == nullptr) return {};
  std::string out;
  while (fgets(buf.data(), static_cast<int>(buf.size()), pipe) != nullptr) {
    out += buf.data();
  }
#ifdef _WIN32
  _pclose(pipe);
#else
  pclose(pipe);
#endif
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
    out.pop_back();
  }
  return out;
}

Options ParseArgs(int argc, char** argv) {
  Options opt;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto need = [&](const char* flag) -> std::string {
      if (i + 1 >= argc) {
        throw std::runtime_error(std::string("Missing value for ") + flag);
      }
      return argv[++i];
    };
    if (arg == "--signal-file") opt.signal_file = need("--signal-file");
    else if (arg == "--ticks-csv") opt.ticks_csv = need("--ticks-csv");
    else if (arg == "--output-signal") opt.output_signal = need("--output-signal");
    else if (arg == "--out-dir") opt.out_dir = need("--out-dir");
    else if (arg == "--artifact-stem") opt.artifact_stem = need("--artifact-stem");
    else if (arg == "--target-column") opt.target_column = need("--target-column");
    else if (arg == "--input-symbol") opt.input_symbol = need("--input-symbol");
    else if (arg == "--iterations") opt.iterations = static_cast<std::size_t>(std::stoull(need("--iterations")));
    else if (arg == "--learning-rate") opt.learning_rate = std::stod(need("--learning-rate"));
    else if (arg == "--beta1") opt.beta1 = std::stod(need("--beta1"));
    else if (arg == "--beta2") opt.beta2 = std::stod(need("--beta2"));
    else if (arg == "--epsilon") opt.epsilon = std::stod(need("--epsilon"));
    else if (arg == "--min-improvement") opt.min_improvement = std::stod(need("--min-improvement"));
    else {
      throw std::runtime_error("Unknown argument: " + arg);
    }
  }
  if (opt.signal_file.empty()) throw std::runtime_error("--signal-file is required");
  if (opt.ticks_csv.empty()) throw std::runtime_error("--ticks-csv is required");
  if (opt.output_signal.empty()) throw std::runtime_error("--output-signal is required");
  if (opt.out_dir.empty()) throw std::runtime_error("--out-dir is required");
  if (opt.iterations == 0) throw std::runtime_error("--iterations must be > 0");
  return opt;
}

std::vector<TickRow> ReadTicksCsv(const std::string& path, const std::string& target_column) {
  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error("Failed to open ticks CSV: " + path);
  }
  std::string header;
  if (!std::getline(in, header)) {
    throw std::runtime_error("ticks CSV is empty: " + path);
  }
  const auto columns = SplitCsvLine(header);
  std::unordered_map<std::string, std::size_t> col;
  for (std::size_t i = 0; i < columns.size(); ++i) {
    col.emplace(columns[i], i);
  }
  for (const char* required : {"bid", "ask"}) {
    if (col.find(required) == col.end()) {
      throw std::runtime_error(std::string("ticks CSV missing column: ") + required);
    }
  }
  if (col.find(target_column) == col.end()) {
    throw std::runtime_error("ticks CSV missing target column: " + target_column);
  }

  auto get_field = [&](const std::vector<std::string>& fields, const std::string& name) -> std::string {
    const auto it = col.find(name);
    if (it == col.end()) return {};
    if (it->second >= fields.size()) {
      throw std::runtime_error("ticks CSV row missing field: " + name);
    }
    return fields[it->second];
  };

  std::vector<TickRow> ticks;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    const auto fields = SplitCsvLine(line);
    TickRow row;
    const std::string ts = get_field(fields, "timestamp_ns");
    if (!ts.empty()) row.timestamp_ns = std::stoull(ts);
    row.bid = std::stod(get_field(fields, "bid"));
    row.ask = std::stod(get_field(fields, "ask"));
    const std::string volume = get_field(fields, "volume");
    if (!volume.empty()) row.volume = std::stod(volume);
    row.target = std::stod(get_field(fields, target_column));
    ticks.push_back(row);
  }
  if (ticks.empty()) {
    throw std::runtime_error("ticks CSV has no data rows: " + path);
  }
  return ticks;
}

PreparedProgram PrepareProgram(
    const std::string& signal_file,
    const std::string& output_signal,
    const std::string& input_symbol) {
  PreparedProgram out;
  out.program = jitse::InlineSignalDependencies(jitse::ParseProgram(ReadFile(signal_file)));
  jitse::AllocateProgramNodeIds(out.program.signals);

  for (const auto& signal : out.program.signals) {
    for (const auto& ticker : jitse::CollectTickerSymbols(signal)) {
      out.symbols.RegisterOrGetId(ticker);
    }
  }
  if (out.program.signals.empty()) {
    throw std::runtime_error("No signals found in program: " + signal_file);
  }
  if (out.symbols.LookupId(input_symbol) == std::numeric_limits<std::size_t>::max()) {
    out.symbols.RegisterOrGetId(input_symbol);
  }
  out.input_symbol_id = static_cast<std::uint32_t>(out.symbols.LookupId(input_symbol));
  for (auto& signal : out.program.signals) {
    jitse::BindSymbolIds(signal, out.symbols);
  }
  for (std::size_t i = 0; i < out.program.signals.size(); ++i) {
    if (out.program.signals[i].name == output_signal) {
      out.output_index = i;
      return out;
    }
  }
  throw std::runtime_error("Output signal not found: " + output_signal);
}

std::vector<double> DefaultParams(const PreparedProgram& program) {
  std::vector<double> params(program.program.params.size(), 0.0);
  for (const auto& param : program.program.params) {
    params[static_cast<std::size_t>(param.param_id)] = param.default_value;
  }
  return params;
}

jitse::MarketState MakeMarket(const TickRow& tick, std::uint32_t input_symbol_id) {
  jitse::MarketState market{};
  auto& inst = market.instruments[input_symbol_id];
  inst.bid = tick.bid;
  inst.ask = tick.ask;
  inst.last_price = 0.5 * (tick.bid + tick.ask);
  inst.volume = tick.volume;
  market.current_time_ns = tick.timestamp_ns;
  return market;
}

EvalResult EvaluateObjectiveAndGradient(
    jitse::JitCompiler::ProgramGradientFn fn,
    const PreparedProgram& program,
    const std::vector<TickRow>& ticks,
    const std::vector<double>& params) {
  if (params.empty()) {
    throw std::runtime_error("Calibration requires at least one parameter");
  }

  EvalResult result;
  result.gradient.assign(params.size(), 0.0);
  const double inv_n = 1.0 / static_cast<double>(ticks.size());
  std::vector<double> outputs(program.program.signals.size(), 0.0);
  std::vector<double> grads(program.program.signals.size(), 0.0);

  for (std::size_t param_index = 0; param_index < params.size(); ++param_index) {
    jitse::MultiSymbolSignalContext arena(1);
    arena.SetParameters(params);
    for (const auto& signal : program.program.signals) {
      jitse::PrewarmSignalContext(arena, 0, signal);
    }
    for (const TickRow& tick : ticks) {
      const jitse::MarketState market = MakeMarket(tick, program.input_symbol_id);
      fn(
          &market,
          &arena,
          0,
          static_cast<std::int64_t>(param_index),
          outputs.data(),
          grads.data());
      const double pred = outputs[program.output_index];
      if (!std::isfinite(pred) || !std::isfinite(grads[program.output_index])) {
        throw std::runtime_error("Calibration hit non-finite compiled output/gradient");
      }
      const double err = pred - tick.target;
      if (param_index == 0) {
        result.objective += 0.5 * err * err * inv_n;
      }
      result.gradient[param_index] += err * grads[program.output_index] * inv_n;
    }
  }
  return result;
}

void WriteTrajectoryCsv(const fs::path& path, const std::vector<double>& trajectory) {
  std::ofstream out(path, std::ios::trunc);
  if (!out) throw std::runtime_error("Failed to open trajectory CSV: " + path.string());
  out << "iteration,objective\n";
  for (std::size_t i = 0; i < trajectory.size(); ++i) {
    out << i << ',' << std::setprecision(17) << trajectory[i] << '\n';
  }
}

void WriteParamsCsv(
    const fs::path& path,
    const std::vector<jitse::ParamDef>& defs,
    const std::vector<double>& initial_params,
    const std::vector<double>& final_params) {
  std::ofstream out(path, std::ios::trunc);
  if (!out) throw std::runtime_error("Failed to open params CSV: " + path.string());
  out << "name,initial,final\n";
  for (std::size_t i = 0; i < defs.size(); ++i) {
    out << defs[i].name << ','
        << std::setprecision(17) << initial_params[i] << ','
        << std::setprecision(17) << final_params[i] << '\n';
  }
}

void WriteReport(
    const fs::path& path,
    const Options& opt,
    const std::string& reproduce_command,
    const std::string& host_fingerprint,
    const std::vector<jitse::ParamDef>& defs,
    const std::vector<double>& initial_params,
    const std::vector<double>& final_params,
    const std::vector<double>& trajectory) {
  std::ofstream out(path, std::ios::trunc);
  if (!out) throw std::runtime_error("Failed to open report: " + path.string());
  out << "# Calibration Fixture Report\n\n";
  out << "- Objective: mean 0.5 * (prediction - target)^2\n";
  out << "- Signal file: `" << opt.signal_file << "`\n";
  out << "- Ticks CSV: `" << opt.ticks_csv << "`\n";
  out << "- Output signal: `" << opt.output_signal << "`\n";
  out << "- Input symbol: `" << opt.input_symbol << "`\n";
  out << "- Optimizer: Adam (`lr=" << opt.learning_rate
      << "`, `beta1=" << opt.beta1
      << "`, `beta2=" << opt.beta2
      << "`, `epsilon=" << opt.epsilon
      << "`, `iterations=" << opt.iterations << "`)\n";
  out << "- Gradient path: `JitCompiler::CompileProgramGradient` / compiled whole-program gradient entrypoint\n";
  out << "- Host fingerprint: `" << host_fingerprint << "`\n";
  out << "- Reproduce: `" << reproduce_command << "`\n\n";

  out << "## Objective\n\n";
  out << "| Initial | Final | Improvement |\n";
  out << "|---|---|---|\n";
  out << "| " << std::setprecision(17) << trajectory.front()
      << " | " << trajectory.back()
      << " | " << (trajectory.front() - trajectory.back()) << " |\n\n";

  out << "## Parameters\n\n";
  out << "| Name | Initial | Final |\n";
  out << "|---|---|---|\n";
  for (std::size_t i = 0; i < defs.size(); ++i) {
    out << "| " << defs[i].name
        << " | " << std::setprecision(17) << initial_params[i]
        << " | " << final_params[i] << " |\n";
  }

  out << "\n## Objective Trajectory\n\n";
  out << "| Iteration | Objective |\n";
  out << "|---|---|\n";
  for (std::size_t i = 0; i < trajectory.size(); ++i) {
    out << "| " << i << " | " << std::setprecision(17) << trajectory[i] << " |\n";
  }
}

void WriteMetaJson(
    const fs::path& path,
    const Options& opt,
    const std::string& reproduce_command,
    const std::string& host_fingerprint,
    const std::string& git_commit,
    const std::vector<jitse::ParamDef>& defs,
    const std::vector<double>& initial_params,
    const std::vector<double>& final_params,
    const std::vector<double>& trajectory) {
  std::ofstream out(path, std::ios::trunc);
  if (!out) throw std::runtime_error("Failed to open meta json: " + path.string());
  out << "{\n";
  out << "  \"artifact\": \"" << JsonEscape(opt.artifact_stem) << "\",\n";
  out << "  \"signal_file\": \"" << JsonEscape(opt.signal_file) << "\",\n";
  out << "  \"ticks_csv\": \"" << JsonEscape(opt.ticks_csv) << "\",\n";
  out << "  \"output_signal\": \"" << JsonEscape(opt.output_signal) << "\",\n";
  out << "  \"input_symbol\": \"" << JsonEscape(opt.input_symbol) << "\",\n";
  out << "  \"objective\": \"mean_half_squared_error\",\n";
  out << "  \"optimizer\": {\n";
  out << "    \"name\": \"adam\",\n";
  out << "    \"learning_rate\": " << std::setprecision(17) << opt.learning_rate << ",\n";
  out << "    \"beta1\": " << opt.beta1 << ",\n";
  out << "    \"beta2\": " << opt.beta2 << ",\n";
  out << "    \"epsilon\": " << opt.epsilon << ",\n";
  out << "    \"iterations\": " << opt.iterations << "\n";
  out << "  },\n";
  out << "  \"gradient_path\": \"compiled_program_gradient\",\n";
  out << "  \"git_commit\": \"" << JsonEscape(git_commit) << "\",\n";
  out << "  \"host_fingerprint\": \"" << JsonEscape(host_fingerprint) << "\",\n";
  out << "  \"reproduce_command\": \"" << JsonEscape(reproduce_command) << "\",\n";
  out << "  \"initial_objective\": " << trajectory.front() << ",\n";
  out << "  \"final_objective\": " << trajectory.back() << ",\n";
  out << "  \"improvement\": " << (trajectory.front() - trajectory.back()) << ",\n";
  out << "  \"parameters\": [\n";
  for (std::size_t i = 0; i < defs.size(); ++i) {
    out << "    {\"name\": \"" << JsonEscape(defs[i].name)
        << "\", \"initial\": " << initial_params[i]
        << ", \"final\": " << final_params[i] << "}";
    out << (i + 1 == defs.size() ? "\n" : ",\n");
  }
  out << "  ],\n";
  out << "  \"trajectory\": [\n";
  for (std::size_t i = 0; i < trajectory.size(); ++i) {
    out << "    " << trajectory[i];
    out << (i + 1 == trajectory.size() ? "\n" : ",\n");
  }
  out << "  ]\n";
  out << "}\n";
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const Options opt = ParseArgs(argc, argv);
    const std::string reproduce_command = JoinCommand(argc, argv);
    const std::vector<TickRow> ticks = ReadTicksCsv(opt.ticks_csv, opt.target_column);
    const PreparedProgram program = PrepareProgram(opt.signal_file, opt.output_signal, opt.input_symbol);

    if (program.program.params.empty()) {
      throw std::runtime_error("Program has no parameters to calibrate");
    }

    jitse::JitCompiler jit;
    if (!jit.IsAvailable()) {
      throw std::runtime_error("LLVM JIT unavailable; calibration requires compiled gradients");
    }
    if (!jit.CompileProgramGradient(program.program.signals, program.symbols)) {
      throw std::runtime_error("CompileProgramGradient failed: " + jit.LastError());
    }
    auto* grad_fn = jit.GetProgramGradientFunction();
    if (grad_fn == nullptr) {
      throw std::runtime_error("CompileProgramGradient returned null function");
    }

    std::vector<double> params = DefaultParams(program);
    const std::vector<double> initial_params = params;
    std::vector<double> m(params.size(), 0.0);
    std::vector<double> v(params.size(), 0.0);
    std::vector<double> trajectory;
    trajectory.reserve(opt.iterations + 1);

    for (std::size_t iter = 0; iter < opt.iterations; ++iter) {
      const EvalResult eval = EvaluateObjectiveAndGradient(grad_fn, program, ticks, params);
      trajectory.push_back(eval.objective);
      std::cout << "iter=" << iter << " objective=" << std::setprecision(17) << eval.objective << "\n";
      const double beta1_pow = std::pow(opt.beta1, static_cast<double>(iter + 1));
      const double beta2_pow = std::pow(opt.beta2, static_cast<double>(iter + 1));
      for (std::size_t i = 0; i < params.size(); ++i) {
        m[i] = opt.beta1 * m[i] + (1.0 - opt.beta1) * eval.gradient[i];
        v[i] = opt.beta2 * v[i] + (1.0 - opt.beta2) * eval.gradient[i] * eval.gradient[i];
        const double m_hat = m[i] / (1.0 - beta1_pow);
        const double v_hat = v[i] / (1.0 - beta2_pow);
        params[i] -= opt.learning_rate * m_hat / (std::sqrt(v_hat) + opt.epsilon);
      }
    }

    const EvalResult final_eval = EvaluateObjectiveAndGradient(grad_fn, program, ticks, params);
    trajectory.push_back(final_eval.objective);
    std::cout << "iter=" << opt.iterations << " objective=" << std::setprecision(17) << final_eval.objective << "\n";

    const double improvement = trajectory.front() - trajectory.back();
    if (!(improvement > 0.0)) {
      throw std::runtime_error("Calibration did not improve the objective");
    }
    if (opt.min_improvement > 0.0 && improvement < opt.min_improvement) {
      std::ostringstream err;
      err << "Objective improvement " << improvement
          << " is below required minimum " << opt.min_improvement;
      throw std::runtime_error(err.str());
    }

    fs::create_directories(opt.out_dir);
    const fs::path out_root(opt.out_dir);
    const fs::path trajectory_csv = out_root / (opt.artifact_stem + "_trajectory.csv");
    const fs::path params_csv = out_root / (opt.artifact_stem + "_params.csv");
    const fs::path report_md = out_root / (opt.artifact_stem + ".md");
    const fs::path meta_json = out_root / (opt.artifact_stem + ".meta.json");

    const std::string host = ExecRead("hostname");
#ifdef _WIN32
    const std::string platform = ExecRead("cmd /c ver");
#else
    const std::string platform = ExecRead("uname -srmo");
#endif
    const std::string git_commit = ExecRead("git rev-parse HEAD 2>/dev/null");
    const std::string host_fingerprint = host.empty() ? platform : host + " | " + platform;

    WriteTrajectoryCsv(trajectory_csv, trajectory);
    WriteParamsCsv(params_csv, program.program.params, initial_params, params);
    WriteReport(
        report_md,
        opt,
        reproduce_command,
        host_fingerprint,
        program.program.params,
        initial_params,
        params,
        trajectory);
    WriteMetaJson(
        meta_json,
        opt,
        reproduce_command,
        host_fingerprint,
        git_commit,
        program.program.params,
        initial_params,
        params,
        trajectory);

    std::cout << "initial_objective=" << std::setprecision(17) << trajectory.front() << "\n";
    std::cout << "final_objective=" << trajectory.back() << "\n";
    std::cout << "objective_improvement=" << improvement << "\n";
    std::cout << "report=" << report_md.string() << "\n";
    std::cout << "meta=" << meta_json.string() << "\n";
    std::cout << "params=" << params_csv.string() << "\n";
    std::cout << "trajectory=" << trajectory_csv.string() << "\n";
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "error: " << ex.what() << "\n";
    return 2;
  }
}
