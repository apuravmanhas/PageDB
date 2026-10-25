#include <iostream>

#include "pagedb/status.h"
#include "pagedb/types.h"

int main() {
  std::cout << "PageDB v" << pagedb::kFormatMajorVersion << "."
            << pagedb::kFormatMinorVersion << " (page_size=" << pagedb::kPageSize
            << "B, max_key=" << pagedb::kMaxKeySize
            << "B, max_val=" << pagedb::kMaxValueSize << "B)\n";
  return 0;
}
