// jitse_fmt -- canonical pretty-printer for the signal DSL (P15).
//
//   jitse_fmt <file>                 # print formatted output to stdout
//   jitse_fmt --in-place <file>      # overwrite file with formatted output
//   jitse_fmt --check <file>         # exit 0 if already formatted, 1 if not
//   jitse_fmt -                      # read from stdin
//
// "Already formatted" means the input is byte-equal to its own format.
// The formatter is idempotent (`fmt(fmt(x)) == fmt(x)`), so it's
// composable with file-watchers / CI hooks the way gofmt is.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

#include "dsl_formatter.h"
#include "parser.h"
#include "signal_program.h"

namespace {

struct Args {
  std::string path;
  bool in_place = false;
  bool check = false;
  bool stdin_input = false;
};

Args ParseArgs(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    const std::string s = argv[i];
    if (s == "--in-place") a.in_place = true;
    else if (s == "--check") a.check = true;
    else if (s == "-h" || s == "--help") {
      std::cout
          << "Usage: jitse_fmt [--in-place|--check] <file>\n"
          << "  jitse_fmt -      read from stdin, write to stdout\n"
          << "  jitse_fmt --check exits 0 if input is already formatted,\n"
          << "                    1 otherwise (no output produced)\n";
      std::exit(0);
    }
    else if (s == "-") { a.stdin_input = true; a.path = "<stdin>"; }
    else if (a.path.empty()) a.path = s;
    else throw std::runtime_error("unexpected extra argument: " + s);
  }
  if (a.path.empty()) {
    throw std::runtime_error("usage: jitse_fmt [--in-place|--check] <file>");
  }
  if (a.in_place && a.check) {
    throw std::runtime_error("--in-place and --check are mutually exclusive");
  }
  if (a.in_place && a.stdin_input) {
    throw std::runtime_error("--in-place requires a path, not stdin");
  }
  return a;
}

std::string ReadStream(std::istream& in) {
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

std::string ReadAll(const Args& args) {
  if (args.stdin_input) return ReadStream(std::cin);
  std::ifstream f(args.path);
  if (!f) throw std::runtime_error("cannot open: " + args.path);
  return ReadStream(f);
}

void WriteAll(const std::string& path, const std::string& contents) {
  // Atomic write: tmpfile + rename, same pattern as the P13 cache.
  std::filesystem::path dst = path;
  std::filesystem::path tmp = dst;
  tmp += ".tmp";
  {
    std::ofstream out(tmp);
    if (!out) throw std::runtime_error("cannot open tmp: " + tmp.string());
    out << contents;
    if (!out) throw std::runtime_error("write failed: " + tmp.string());
  }
  std::error_code ec;
  std::filesystem::rename(tmp, dst, ec);
  if (ec) {
    std::filesystem::remove(tmp);
    throw std::runtime_error("rename failed: " + ec.message());
  }
}

}  // namespace

int main(int argc, char** argv) try {
  const Args args = ParseArgs(argc, argv);
  const std::string src = ReadAll(args);

  // ParseSignalProgram does line-stamped error rendering with a
  // caret -- propagate that out unchanged. We deliberately do NOT
  // run type-checking here; that's `jitse_lint`'s job. The
  // formatter only needs the AST to be parseable.
  std::vector<jitse::SignalDef> parsed = jitse::ParseSignalProgram(src);
  const std::string formatted = jitse::FormatProgram(parsed);

  if (args.check) {
    if (formatted == src) {
      return 0;
    }
    std::cerr << args.path << ": needs formatting (run `jitse_fmt --in-place "
              << args.path << "` to fix)\n";
    return 1;
  }

  if (args.in_place) {
    if (formatted != src) {
      WriteAll(args.path, formatted);
      std::cerr << "rewrote " << args.path << "\n";
    }
    return 0;
  }

  // Default: print formatted output to stdout.
  std::cout << formatted;
  return 0;
} catch (const jitse::ParseError& e) {
  std::cerr << e.what() << "\n";
  return 2;
} catch (const std::exception& e) {
  std::cerr << "error: " << e.what() << "\n";
  return 2;
}
