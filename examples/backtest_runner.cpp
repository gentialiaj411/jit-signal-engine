#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <cstdio>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ast_utils.h"
#include "interpreter.h"
#include "jit_compiler.h"
#include "runtime.h"
#include "signal_program.h"

#include "mf/core/crc32.hpp"
#include "mf/core/types.hpp"
#include "mf/journal/journal_reader.hpp"

namespace fs = std::filesystem;

namespace {

struct Options {
  std::string journal_path;
  std::string events_csv_path;
  std::string signal_file{"examples/filtered_momentum.sig"};
  std::string out_root{"bench/results/backtest"};
  std::string run_id{"run_default"};
  std::uint64_t max_events{5000000};
  std::uint64_t seed{42};
};

struct IcResult {
  double ic1 = std::numeric_limits<double>::quiet_NaN();
  double ic5 = std::numeric_limits<double>::quiet_NaN();
  double ic30 = std::numeric_limits<double>::quiet_NaN();
  std::size_t n1 = 0;
  std::size_t n5 = 0;
  std::size_t n30 = 0;
};

struct ReplayEvent {
  std::uint64_t timestamp_ns = 0;
  std::string symbol;
  std::string event_type;
  std::string side;
  std::int64_t price = 0;
  std::int64_t qty = 0;
};

std::string ReadFile(const std::string& path) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("Failed to open: " + path);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string SymbolText(const mf::core::SymbolKey& key) {
  std::string out;
  out.reserve(key.bytes.size());
  for (char c : key.bytes) {
    if (c == '\0' || c == ' ') continue;
    out.push_back(c);
  }
  return out;
}

std::string ResolveSymbol(
    const mf::core::BookEvent& ev,
    std::unordered_map<std::uint64_t, std::string>& order_to_symbol) {
  const std::string symbol = SymbolText(ev.symbol);
  if (!symbol.empty()) {
    if (ev.order_id != 0) {
      order_to_symbol[ev.order_id] = symbol;
    }
    if (ev.reference_order_id != 0) {
      order_to_symbol[ev.reference_order_id] = symbol;
    }
    return symbol;
  }

  auto remember = [&](std::uint64_t order_id) -> std::string {
    if (order_id == 0) return "UNKNOWN";
    const auto it = order_to_symbol.find(order_id);
    if (it == order_to_symbol.end()) return "UNKNOWN";
    return it->second;
  };

  switch (ev.type) {
    case mf::core::EventType::Execute:
    case mf::core::EventType::ExecutePrice:
    case mf::core::EventType::Cancel:
    case mf::core::EventType::Delete:
    case mf::core::EventType::Trade:
    case mf::core::EventType::CrossTrade:
      return remember(ev.order_id);
    case mf::core::EventType::Replace: {
      const std::string resolved = remember(ev.reference_order_id);
      if (resolved != "UNKNOWN" && ev.order_id != 0) {
        order_to_symbol[ev.order_id] = resolved;
      }
      return resolved;
    }
    default:
      return "UNKNOWN";
  }
}

double Pearson(const std::vector<double>& x, const std::vector<double>& y) {
  if (x.size() != y.size() || x.size() < 2) return std::numeric_limits<double>::quiet_NaN();
  long double sx = 0.0L;
  long double sy = 0.0L;
  long double sxx = 0.0L;
  long double syy = 0.0L;
  long double sxy = 0.0L;
  for (std::size_t i = 0; i < x.size(); ++i) {
    const long double xi = static_cast<long double>(x[i]);
    const long double yi = static_cast<long double>(y[i]);
    sx += xi;
    sy += yi;
    sxx += xi * xi;
    syy += yi * yi;
    sxy += xi * yi;
  }
  const long double n = static_cast<long double>(x.size());
  const long double cov = sxy - (sx * sy / n);
  const long double vx = sxx - (sx * sx / n);
  const long double vy = syy - (sy * sy / n);
  if (vx <= 0.0L || vy <= 0.0L) return std::numeric_limits<double>::quiet_NaN();
  return static_cast<double>(cov / std::sqrt(vx * vy));
}

std::uint32_t FileCrc32(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return 0;
  constexpr std::size_t kBuf = 1U << 16U;
  std::vector<std::byte> buf(kBuf);
  std::uint32_t crc = 0;
  while (in) {
    in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    const auto got = static_cast<std::size_t>(in.gcount());
    if (got == 0) break;
    crc = mf::core::crc32_update(crc, buf.data(), got);
  }
  return crc;
}

std::vector<std::string> SplitCsvLine(const std::string& line) {
  std::vector<std::string> fields;
  std::string field;
  std::istringstream in(line);
  while (std::getline(in, field, ',')) {
    while (!field.empty() && (field.back() == '\r' || field.back() == '\n')) field.pop_back();
    fields.push_back(field);
  }
  return fields;
}

std::vector<ReplayEvent> ReadReplayCsv(const std::string& path, std::uint64_t max_events) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("Failed to open events CSV: " + path);

  std::string header;
  if (!std::getline(in, header)) throw std::runtime_error("events CSV is empty: " + path);
  const auto columns = SplitCsvLine(header);
  std::unordered_map<std::string, std::size_t> col;
  for (std::size_t i = 0; i < columns.size(); ++i) col.emplace(columns[i], i);
  for (const char* required : {"timestamp_ns", "symbol", "event_type", "side", "price", "qty"}) {
    if (col.find(required) == col.end()) throw std::runtime_error(std::string("events CSV missing column: ") + required);
  }

  std::vector<ReplayEvent> events;
  std::string line;
  while ((max_events == 0 || events.size() < max_events) && std::getline(in, line)) {
    if (line.empty()) continue;
    const auto fields = SplitCsvLine(line);
    auto get = [&](const char* name) -> const std::string& {
      const std::size_t i = col.at(name);
      if (i >= fields.size()) throw std::runtime_error(std::string("events CSV row missing field: ") + name);
      return fields[i];
    };
    ReplayEvent ev;
    ev.timestamp_ns = std::stoull(get("timestamp_ns"));
    ev.symbol = get("symbol");
    ev.event_type = get("event_type");
    ev.side = get("side");
    ev.price = std::stoll(get("price"));
    ev.qty = std::stoll(get("qty"));
    events.push_back(std::move(ev));
  }
  return events;
}

std::string ExecRead(const char* cmd) {
  std::array<char, 256> buf{};
  std::string out;
  FILE* f = popen(cmd, "r");
  if (f == nullptr) return out;
  while (fgets(buf.data(), static_cast<int>(buf.size()), f) != nullptr) {
    out += buf.data();
  }
  pclose(f);
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
  return out;
}

Options ParseArgs(int argc, char** argv) {
  Options opt;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto need = [&](const char* name) -> std::string {
      if (i + 1 >= argc) throw std::runtime_error(std::string("missing value for ") + name);
      return argv[++i];
    };
    if (a == "--journal") opt.journal_path = need("--journal");
    else if (a == "--events-csv") opt.events_csv_path = need("--events-csv");
    else if (a == "--signal-file") opt.signal_file = need("--signal-file");
    else if (a == "--out-root") opt.out_root = need("--out-root");
    else if (a == "--run-id") opt.run_id = need("--run-id");
    else if (a == "--max-events") opt.max_events = std::stoull(need("--max-events"));
    else if (a == "--seed") opt.seed = std::stoull(need("--seed"));
  }
  if (opt.journal_path.empty() == opt.events_csv_path.empty()) {
    throw std::runtime_error("exactly one of --journal or --events-csv is required");
  }
  return opt;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const Options opt = ParseArgs(argc, argv);

    std::vector<jitse::SignalDef> signals = jitse::ParseSignalProgram(ReadFile(opt.signal_file));

    jitse::SymbolTable symbols;
    std::vector<std::string> tickers;
    std::unordered_map<std::string, std::uint32_t> ticker_to_slot;
    for (const auto& s : signals) {
      const auto ts = jitse::CollectTickerSymbols(s);
      tickers.insert(tickers.end(), ts.begin(), ts.end());
    }
    if (tickers.empty()) tickers.push_back("AAPL");
    for (const auto& t : tickers) {
      const std::uint32_t slot = static_cast<std::uint32_t>(symbols.RegisterOrGetId(t));
      ticker_to_slot.emplace(t, slot);
    }
    jitse::AllocateProgramNodeIds(signals);
    for (auto& s : signals) {
      jitse::BindSymbolIds(s, symbols);
    }

    jitse::JitCompiler jit;
    if (!jit.IsAvailable()) throw std::runtime_error("LLVM JIT unavailable");
    if (!jit.CompileProgram(signals, symbols)) throw std::runtime_error("CompileProgram failed: " + jit.LastError());
    auto fn = jit.GetProgramFunction();
    if (fn == nullptr) throw std::runtime_error("Program function is null");

    std::unordered_map<std::string, std::uint32_t> symbol_to_id;
    std::vector<std::string> id_to_symbol;
    std::unordered_map<std::uint64_t, std::string> order_to_symbol;
    jitse::MarketState market{};
    jitse::MultiSymbolSignalContext ctx(0);
    std::vector<std::vector<double>> mid_series;
    std::vector<std::vector<std::vector<double>>> signal_series;
    std::vector<double> outputs(signals.size(), 0.0);

    const fs::path run_dir = fs::path(opt.out_root) / opt.run_id;
    fs::create_directories(run_dir);
    std::ofstream sig_out(run_dir / "signals.csv", std::ios::trunc);
    if (!sig_out) throw std::runtime_error("failed to open signals.csv");
    sig_out << "timestamp_ns,symbol,signal,value\n";

    mf::core::BookEvent ev{};
    std::uint64_t ingest_ts_ns = 0;
    std::uint64_t journal_seq = 0;
    std::uint64_t processed = 0;

    auto process_event = [&](const ReplayEvent& rev) {
      const std::string sym = rev.symbol.empty() ? "UNKNOWN" : rev.symbol;
      auto it = symbol_to_id.find(sym);
      std::uint32_t sid = 0;
      if (it == symbol_to_id.end()) {
        sid = static_cast<std::uint32_t>(id_to_symbol.size());
        symbol_to_id.emplace(sym, sid);
        id_to_symbol.push_back(sym);
        ctx.Resize(id_to_symbol.size());
        for (const auto& s : signals) jitse::PrewarmSignalContext(ctx, sid, s);
        mid_series.emplace_back();
        signal_series.emplace_back(signals.size());
      } else {
        sid = it->second;
      }

      const auto input_it = ticker_to_slot.find(sym);
      if (input_it != ticker_to_slot.end()) {
        auto& inst = market.instruments[input_it->second];
        const double px = static_cast<double>(rev.price) * 1e-4;
        if (px > 0.0) {
          if (rev.side == "Buy") inst.bid = px;
          if (rev.side == "Sell") inst.ask = px;
          if (rev.event_type == "Trade" || rev.event_type == "CrossTrade") {
            inst.last_price = px;
            if (inst.bid <= 0.0) inst.bid = px;
            if (inst.ask <= 0.0) inst.ask = px;
          }
        }
        inst.volume = static_cast<double>(rev.qty);
      }
      market.current_time_ns = rev.timestamp_ns;

      fn(&market, &ctx, sid, outputs.data());

      double mid = 0.0;
      const auto input_slot = ticker_to_slot.find("AAPL");
      if (input_slot != ticker_to_slot.end()) {
        const auto& inst = market.instruments[input_slot->second];
        if (inst.bid > 0.0 && inst.ask > 0.0) mid = 0.5 * (inst.bid + inst.ask);
        else if (inst.bid > 0.0) mid = inst.bid;
        else if (inst.ask > 0.0) mid = inst.ask;
        else mid = inst.last_price;
      }

      mid_series[sid].push_back(mid);
      for (std::size_t i = 0; i < outputs.size(); ++i) {
        signal_series[sid][i].push_back(outputs[i]);
        sig_out << market.current_time_ns << ',' << sym << ',' << signals[i].name << ',' << std::setprecision(17) << outputs[i] << '\n';
      }

      ++processed;
    };

    if (!opt.events_csv_path.empty()) {
      for (const ReplayEvent& rev : ReadReplayCsv(opt.events_csv_path, opt.max_events)) {
        process_event(rev);
      }
    } else {
      mf::journal::JournalReader reader;
      if (!reader.open(opt.journal_path)) {
        throw std::runtime_error("Failed to open journal: " + opt.journal_path);
      }
      while (processed < opt.max_events && reader.next(ev, ingest_ts_ns, journal_seq)) {
        (void)journal_seq;
        process_event(ReplayEvent{
            (ev.exchange_ts_ns != 0) ? ev.exchange_ts_ns : ingest_ts_ns,
            ResolveSymbol(ev, order_to_symbol),
            (ev.type == mf::core::EventType::Trade) ? "Trade" :
                (ev.type == mf::core::EventType::CrossTrade) ? "CrossTrade" : "Other",
            (ev.side == mf::core::Side::Buy) ? "Buy" :
                (ev.side == mf::core::Side::Sell) ? "Sell" : "Unknown",
            static_cast<std::int64_t>(ev.price),
            static_cast<std::int64_t>(ev.qty)});
      }
    }

    std::vector<IcResult> per_signal(signals.size());
    auto compute_for_h = [&](std::size_t h, auto setter_ic, auto setter_n) {
      for (std::size_t si = 0; si < signals.size(); ++si) {
        std::vector<double> xs;
        std::vector<double> ys;
        for (std::size_t sid = 0; sid < signal_series.size(); ++sid) {
          const auto& mids = mid_series[sid];
          const auto& sigv = signal_series[sid][si];
          if (mids.size() <= h || sigv.size() <= h) continue;
          const std::size_t n = std::min(mids.size(), sigv.size()) - h;
          xs.reserve(xs.size() + n);
          ys.reserve(ys.size() + n);
          for (std::size_t t = 0; t < n; ++t) {
            const double m0 = mids[t];
            const double m1 = mids[t + h];
            if (!(m0 > 0.0) || !std::isfinite(sigv[t]) || !std::isfinite(m1)) continue;
            const double ret = (m1 - m0) / m0;
            xs.push_back(sigv[t]);
            ys.push_back(ret);
          }
        }
        const double ic = Pearson(xs, ys);
        setter_ic(per_signal[si], ic);
        setter_n(per_signal[si], xs.size());
      }
    };

    compute_for_h(1,
        [](IcResult& r, double v) { r.ic1 = v; },
        [](IcResult& r, std::size_t n) { r.n1 = n; });
    compute_for_h(5,
        [](IcResult& r, double v) { r.ic5 = v; },
        [](IcResult& r, std::size_t n) { r.n5 = n; });
    compute_for_h(30,
        [](IcResult& r, double v) { r.ic30 = v; },
        [](IcResult& r, std::size_t n) { r.n30 = n; });

    const std::string data_source = opt.events_csv_path.empty() ? opt.journal_path : opt.events_csv_path;
    const std::uint32_t src_crc = FileCrc32(data_source);

    std::ofstream ic_out(run_dir / "ic_report.json", std::ios::trunc);
    if (!ic_out) throw std::runtime_error("failed to open ic_report.json");

    ic_out << "{\n";
    ic_out << "  \"run_metadata\": {\n";
    const std::string commit = ExecRead("git rev-parse HEAD 2>/dev/null");
    const std::string date_utc = ExecRead("date -u +%Y-%m-%dT%H:%M:%SZ");
    ic_out << "    \"commit\": \"" << (commit.empty() ? "unknown" : commit) << "\",\n";
    ic_out << "    \"host\": \"wsl\",\n";
    ic_out << "    \"date\": \"" << (date_utc.empty() ? "unknown" : date_utc) << "\",\n";
    ic_out << "    \"seed\": " << opt.seed << ",\n";
    ic_out << "    \"data_source_path\": \"" << data_source << "\",\n";
    ic_out << "    \"data_source_hash\": \"0x" << std::hex << std::setw(8) << std::setfill('0') << src_crc << std::dec << "\",\n";
    ic_out << "    \"signal_program_path\": \"" << opt.signal_file << "\",\n";
    ic_out << "    \"n_symbols\": " << id_to_symbol.size() << ",\n";
    ic_out << "    \"n_events\": " << processed << "\n";
    ic_out << "  },\n";

    auto write_map = [&](const char* key, auto val_fn) {
      ic_out << "  \"" << key << "\": {\n";
      for (std::size_t i = 0; i < signals.size(); ++i) {
        ic_out << "    \"" << signals[i].name << "\": " << std::setprecision(17) << val_fn(per_signal[i]);
        ic_out << ((i + 1 == signals.size()) ? "\n" : ",\n");
      }
      ic_out << "  },\n";
    };

    write_map("per_signal_ic_at_horizon_1", [](const IcResult& r) { return r.ic1; });
    write_map("per_signal_ic_at_horizon_5", [](const IcResult& r) { return r.ic5; });
    write_map("per_signal_ic_at_horizon_30", [](const IcResult& r) { return r.ic30; });

    ic_out << "  \"per_signal_sample_size\": {\n";
    for (std::size_t i = 0; i < signals.size(); ++i) {
      ic_out << "    \"" << signals[i].name << "\": {\"h1\": " << per_signal[i].n1
             << ", \"h5\": " << per_signal[i].n5
             << ", \"h30\": " << per_signal[i].n30 << "}";
      ic_out << ((i + 1 == signals.size()) ? "\n" : ",\n");
    }
    ic_out << "  },\n";

    ic_out << "  \"per_signal_p_value\": null,\n";
    ic_out << "  \"per_signal_p_value_reason\": \"not_computed\"\n";
    ic_out << "}\n";

    std::cout << "data_source=" << data_source << "\n";
    std::cout << "events=" << processed << "\n";
    std::cout << "symbols=" << id_to_symbol.size() << "\n";
    std::cout << "ic_report=" << (run_dir / "ic_report.json").string() << "\n";
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "error: " << ex.what() << "\n";
    return 2;
  }
}
