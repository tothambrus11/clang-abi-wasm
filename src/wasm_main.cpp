// Emscripten needs a main(); the module is driven through the exported
// abi_query symbol rather than argv, so this only keeps the runtime alive.
int main() { return 0; }
