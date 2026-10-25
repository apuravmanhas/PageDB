#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "pagedb/database.h"
#include "pagedb/status.h"
#include "pagedb/types.h"

namespace {

void print_banner() {
  std::cout << "PageDB v" << pagedb::kFormatMajorVersion << "."
            << pagedb::kFormatMinorVersion << " (page_size=" << pagedb::kPageSize
            << "B, max_key=" << pagedb::kMaxKeySize
            << "B, max_val=" << pagedb::kMaxValueSize << "B)\n";
}

void print_help() {
  std::cout << "Commands:\n"
            << "  put <key> <value>        Insert or update a key-value pair\n"
            << "  get <key>                Lookup value for key\n"
            << "  del <key>                Delete key\n"
            << "  scan [start] [end]       Range scan [start, end)\n"
            << "  stats                    Display storage and buffer pool stats\n"
            << "  verify                   Run structural B+ tree and free-list verification\n"
            << "  sync                     Flush dirty pages and fsync data file\n"
            << "  help                     Show this command reference\n"
            << "  quit / exit              Flush, close, and exit\n";
}

bool execute_tokens(pagedb::Database& db,
                    const std::vector<std::string>& tokens,
                    bool& should_exit) {
  if (tokens.empty()) {
    return true;
  }
  const std::string& cmd = tokens[0];
  if (cmd == "quit" || cmd == "exit") {
    should_exit = true;
    return true;
  }
  if (cmd == "help") {
    print_help();
    return true;
  }
  if (cmd == "put") {
    if (tokens.size() != 3) {
      std::cerr << "ERR usage: put <key> <value>\n";
      return false;
    }
    pagedb::Status s = db.put(tokens[1], tokens[2]);
    if (!s.ok()) {
      std::cerr << "ERR " << s.to_string() << "\n";
      return false;
    }
    std::cout << "OK\n";
    return true;
  }
  if (cmd == "get") {
    if (tokens.size() != 2) {
      std::cerr << "ERR usage: get <key>\n";
      return false;
    }
    auto res = db.get(tokens[1]);
    if (res.code() == pagedb::ErrorCode::kNotFound) {
      std::cout << "(nil)\n";
      return true;
    }
    if (!res.ok()) {
      std::cerr << "ERR " << res.status().to_string() << "\n";
      return false;
    }
    std::cout << res.value() << "\n";
    return true;
  }
  if (cmd == "del") {
    if (tokens.size() != 2) {
      std::cerr << "ERR usage: del <key>\n";
      return false;
    }
    auto res = db.del(tokens[1]);
    if (!res.ok()) {
      std::cerr << "ERR " << res.status().to_string() << "\n";
      return false;
    }
    std::cout << (res.value() ? "1\n" : "0\n");
    return true;
  }
  if (cmd == "scan") {
    if (tokens.size() > 3) {
      std::cerr << "ERR usage: scan [start_key] [end_key]\n";
      return false;
    }
    std::optional<std::string_view> start_k;
    std::optional<std::string_view> end_k;
    if (tokens.size() >= 2) {
      start_k = tokens[1];
    }
    if (tokens.size() == 3) {
      end_k = tokens[2];
    }
    auto res = db.scan(start_k, end_k);
    if (!res.ok()) {
      std::cerr << "ERR " << res.status().to_string() << "\n";
      return false;
    }
    for (const auto& kv : res.value()) {
      std::cout << kv.key << " => " << kv.value << "\n";
    }
    std::cout << "(" << res.value().size() << " entries)\n";
    return true;
  }
  if (cmd == "stats") {
    const auto bp = db.buffer_pool_stats();
    const auto h_res = db.tree_height();
    std::cout << "page_count: " << db.page_count() << "\n"
              << "root_page_id: " << db.root_page_id().value << "\n"
              << "tree_height: " << (h_res.ok() ? h_res.value() : 0U) << "\n"
              << "free_list_head: " << db.free_list_head().value << "\n"
              << "bp_hits: " << bp.hits << "\n"
              << "bp_misses: " << bp.misses << "\n"
              << "bp_evictions: " << bp.evictions << "\n"
              << "bp_dirty_flushes: " << bp.dirty_flushes << "\n";
    return true;
  }
  if (cmd == "verify") {
    pagedb::Status s = db.verify();
    if (!s.ok()) {
      std::cerr << "ERR " << s.to_string() << "\n";
      return false;
    }
    std::cout << "OK (verified)\n";
    return true;
  }
  if (cmd == "sync") {
    pagedb::Status s = db.sync();
    if (!s.ok()) {
      std::cerr << "ERR " << s.to_string() << "\n";
      return false;
    }
    std::cout << "OK (synced)\n";
    return true;
  }

  std::cerr << "ERR unknown command: " << cmd << "\n";
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc <= 1) {
    print_banner();
    std::cout << "Usage: pagedb_cli <db_dir> [command args...]\n";
    return 0;
  }

  const std::string_view arg1(argv[1]);
  if (arg1 == "--version" || arg1 == "-v") {
    print_banner();
    return 0;
  }
  if (arg1 == "--help" || arg1 == "-h") {
    print_banner();
    print_help();
    return 0;
  }

  auto db_res = pagedb::Database::open(argv[1]);
  if (!db_res.ok()) {
    std::cerr << "ERR failed to open database: "
              << db_res.status().to_string() << "\n";
    return 1;
  }
  pagedb::Database& db = *db_res.value();

  // One-shot command mode.
  if (argc > 2) {
    std::vector<std::string> tokens;
    for (int i = 2; i < argc; ++i) {
      tokens.emplace_back(argv[i]);
    }
    bool should_exit = false;
    const bool ok = execute_tokens(db, tokens, should_exit);
    pagedb::Status close_s = db.close();
    if (!close_s.ok()) {
      std::cerr << "ERR close failed: " << close_s.to_string() << "\n";
      return 1;
    }
    return ok ? 0 : 1;
  }

  // Interactive REPL mode.
  print_banner();
  std::string line;
  while (true) {
    std::cout << "pagedb> " << std::flush;
    if (!std::getline(std::cin, line)) {
      std::cout << "\n";
      break;
    }
    std::istringstream iss(line);
    std::vector<std::string> tokens;
    std::string tok;
    while (iss >> tok) {
      tokens.push_back(std::move(tok));
    }
    bool should_exit = false;
    (void)execute_tokens(db, tokens, should_exit);
    if (should_exit) {
      break;
    }
  }

  pagedb::Status close_s = db.close();
  if (!close_s.ok()) {
    std::cerr << "ERR close failed: " << close_s.to_string() << "\n";
    return 1;
  }
  return 0;
}
