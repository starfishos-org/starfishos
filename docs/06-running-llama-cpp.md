# Running llama.cpp

StarfishOS builds the current `llama.cpp` checkout as a static, CPU-only demo.
The port disables native host probing, OpenMP, Linux-specific llamafile
kernels, dynamic backend discovery, model mmap, and GPU backends so that the
result is deterministic under the ChCore musl toolchain. Model data uses
buffered hostfs reads. The `llama-simple` executable is copied into the ramdisk.

## Build

Initialize the upstream checkout and enable the demo in `.config`:

```bash
git submodule update --init --recursive -- user/demos/llama.cpp
./chbuild defconfig
sed -i 's/CHCORE_DEMOS_LLAMACPP:BOOL=OFF/CHCORE_DEMOS_LLAMACPP:BOOL=ON/' .config
./chbuild build
```

## Prepare a smoke-test model

The tiny 15M-parameter model is useful for validating the port, not for
measuring model quality. Download it through the Hugging Face mirror:

```bash
mkdir -p log/llama-cpp/model
curl -fL \
  https://hf-mirror.com/ggml-org/tiny-llamas/resolve/main/stories15M-q4_0.gguf \
  -o log/llama-cpp/model/stories15M-q4_0.gguf
```

Expose the GGUF through hostfs before starting QEMU:

```bash
CHCORE_HOSTFS_FILES=log/llama-cpp/model/stories15M-q4_0.gguf \
  python dsm-scripts/prepare_hostfs.py
```

The basename becomes the guest-visible path under `/host`. Run one CPU-only
generation as a smoke test:

```text
llama-simple -m /host/stories15M-q4_0.gguf -ngl 0 -n 4 "Once upon a time"
```
