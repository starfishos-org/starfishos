#include "llama.h"

// Static builds already register the CPU backend. The upstream example still
// scans the executable directory for dynamic backends, which ChCore's minimal
// filesystem layer does not support yet.
#define ggml_backend_load_all() ((void) 0)

// hostfs supports ordinary reads, but its whole-file mmap compatibility does
// not yet provide the mapping lifecycle expected by llama.cpp model buffers.
// Force the loader down the buffered I/O path.
static llama_model_params llama_chcore_model_default_params() {
    llama_model_params params = ::llama_model_default_params();
    params.load_mode = LLAMA_LOAD_MODE_NONE;
    return params;
}

#define llama_model_default_params() llama_chcore_model_default_params()
#include "llama.cpp/examples/simple/simple.cpp"
