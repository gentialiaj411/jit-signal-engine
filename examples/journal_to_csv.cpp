#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unordered_map>
#include <stdexcept>
#include <string>

#include "mf/core/types.hpp"
#include "mf/journal/journal_reader.hpp"

namespace fs = std::filesystem;

namespace {

struct Options {
  std::string journal_path;
  std::string out_csv;
  std::uint64_t max_events{0};
};

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

std::string ToString(mf::core::EventType type) {
  switch (type) {
    case mf::core::EventType::Add:
      return "Add";
    case mf::core::EventType::AddMpid:
      return "AddMpid";
    case mf::core::EventType::Execute:
      return "Execute";
    case mf::core::EventType::ExecutePrice:
      return "ExecutePrice";
    case mf::core::EventType::Cancel:
      return "Cancel";
    case mf::core::EventType::Delete:
      return "Delete";
    case mf::core::EventType::Replace:
      return "Replace";
    case mf::core::EventType::Trade:
      return "Trade";
    case mf::core::EventType::CrossTrade:
      return "CrossTrade";
    case mf::core::EventType::Imbalance:
      return "Imbalance";
    case mf::core::EventType::System:
      return "System";
    case mf::core::EventType::StockDirectory:
      return "StockDirectory";
    case mf::core::EventType::Unknown:
    default:
      return "Unknown";
  }
}

std::string ToString(mf::core::Side side) {
  switch (side) {
    case mf::core::Side::Buy:
      return "Buy";
    case mf::core::Side::Sell:
      return "Sell";
    case mf::core::Side::Unknown:
    default:
      return "Unknown";
  }
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
    else if (a == "--out-csv") opt.out_csv = need("--out-csv");
    else if (a == "--max-events") opt.max_events = std::stoull(need("--max-events"));
  }
  if (opt.journal_path.empty()) throw std::runtime_error("--journal is required");
  if (opt.out_csv.empty()) throw std::runtime_error("--out-csv is required");
  return opt;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const Options opt = ParseArgs(argc, argv);

    mf::journal::JournalReader reader;
    if (!reader.open(opt.journal_path)) {
      throw std::runtime_error("failed to open journal: " + opt.journal_path);
    }

    fs::path out_path(opt.out_csv);
    if (out_path.has_parent_path()) {
      fs::create_directories(out_path.parent_path());
    }

    std::ofstream out(opt.out_csv, std::ios::trunc);
    if (!out) {
      throw std::runtime_error("failed to open output csv: " + opt.out_csv);
    }
    out << "event_index,sequence,timestamp_ns,exchange_ts_ns,ingest_ts_ns,symbol,event_type,side,price,qty\n";

    mf::core::BookEvent ev{};
    std::uint64_t ingest_ts_ns = 0;
    std::uint64_t journal_seq = 0;
    std::uint64_t processed = 0;
    std::unordered_map<std::uint64_t, std::string> order_to_symbol;

    while ((opt.max_events == 0 || processed < opt.max_events) && reader.next(ev, ingest_ts_ns, journal_seq)) {
      const std::uint64_t timestamp_ns = (ev.exchange_ts_ns != 0) ? ev.exchange_ts_ns : ingest_ts_ns;
      out << processed << ','
          << journal_seq << ','
          << timestamp_ns << ','
          << ev.exchange_ts_ns << ','
          << ingest_ts_ns << ','
          << ResolveSymbol(ev, order_to_symbol) << ','
          << ToString(ev.type) << ','
          << ToString(ev.side) << ','
          << ev.price << ','
          << ev.qty << '\n';
      ++processed;
    }

    std::cout << "journal=" << opt.journal_path << "\n";
    std::cout << "events=" << processed << "\n";
    std::cout << "out_csv=" << opt.out_csv << "\n";
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "error: " << ex.what() << "\n";
    return 2;
  }
}
