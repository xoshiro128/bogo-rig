# [bogo-rig]
bogo client for [bogo.swapjs.dev](https://bogo.swapjs.dev) written in C with multiple backends (CPU and GPU)

Linux native, for Windows you would use WSL2

## Requirements
* **Compiler**: `gcc` or `clang`, `nvcc` for CUDA, `hipcc` for HIP, `glslc` and `xxd` for Vulkan.
* **Dependencies**: `libwebsockets`, `pthread`.

## Building
Run `make BACKEND=CPU/CUDA/HIP/VULKAN` 
* By default `make` will compile for CPU backend, if you want GPU acceleration specify CUDA for NVIDIA graphic cards and HIP for AMD graphic cards, or VULKAN for other vendors GPUs and iGPUs.

## Usage
Run with `./bogowsclient --uuid <your uuid> --code <your code> --nickname <your bogo username> --workers <amount of worker threads> --chunk-size <shuffles assigned per job>`
* `--workers` if not specified defaults to 1
* `--chunk-size` if not specified defaults to 1 million
* You can find your account uuid, code and nickname by running `localStorage` in the web console while logged into your account

### Drivers
* For a GPU backend, make sure your GPU drivers are up to date and support the necessary APIs (NVIDIA drivers for CUDA, ROCm-compatible drivers for HIP, or Vulkan-capable drivers).

## License
This project is licensed under the MIT License, see the LICENSE file for details.
