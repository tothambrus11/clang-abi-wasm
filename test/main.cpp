// Native harness: reads an AbiRequest on stdin, writes the AbiResponse on
// stdout. Same entry point the wasm module exports, so anything that
// reproduces here reproduces there — and this one runs in milliseconds.
//
//   echo '{"triple":"x86_64-unknown-linux-gnu","source":"struct S{char a;int b;};"}' \
//     | ./abi_query_test | jq .
#include "abi_query.h"
#include <iostream>
#include <iterator>
#include <string>

int main() {
  std::string request{std::istreambuf_iterator<char>(std::cin),
                      std::istreambuf_iterator<char>()};
  std::cout << abi_query(request.c_str()) << '\n';
  return 0;
}
