#include "adios2/common/ADIOSTypes.h"

#include <adios2.h>
#include <cuda_runtime.h>
#include <cuda_runtime_api.h>
#include <iostream>

using value_type = long;

__global__ void __cuda_init_array(value_type *devPtr, size_t width, size_t height, size_t pitch)
{
    size_t idx = sizeof(value_type) * threadIdx.x + threadIdx.y * pitch;
    auto val = threadIdx.y * width + threadIdx.x;
    reinterpret_cast<value_type &>((reinterpret_cast<char *>(devPtr))[idx]) =
        static_cast<value_type>(val);
}

void cuda_init_array(value_type *devPtr, size_t width, size_t height, size_t pitch)
{
    dim3 threadsPerBlock(width, height);
    std::cout << "WIDTH:\t" << width << "\tHEIGHT:\t" << height << std::endl;
    __cuda_init_array<<<1, threadsPerBlock>>>(devPtr, width, height, pitch);
}

auto main() -> int
{
    constexpr size_t width = 10;
    constexpr size_t height = 20;

    void *devPtr;
    size_t pitch;
    cudaMallocPitch(&devPtr, &pitch, width * sizeof(value_type), height);
    cuda_init_array(static_cast<value_type *>(devPtr), width, height, pitch);
    std::cout << "Pitch: " << pitch / sizeof(value_type) << '\n';

    adios2::ADIOS adios;
    auto IO = adios.DeclareIO("asdfg");
    IO.SetEngine("bp5");
    auto engine = IO.Open("MemSelection.bp", adios2::Mode::Write);
    engine.BeginStep();

    {
        auto var = IO.DefineVariable<value_type>("gpu_pointer_without_mem_selection",
                                                 {height, width}, {0, 0}, {height, width});
        engine.Put(var, static_cast<value_type const *>(devPtr));
    }

    /*
     * This is the logic that does not work.
     */
    {
        auto var = IO.DefineVariable<value_type>("gpu_pointer_with_mem_selection", {height, width},
                                                 {0, 0}, {height, width});
        var.SetMemorySelection({{0, 0}, {height, pitch / sizeof(value_type)}});
        engine.Put(var, static_cast<value_type const *>(devPtr));
    }

    size_t flattened_non_paddened_extent = height * width;
    std::unique_ptr<value_type[]> hostPtr{new value_type[flattened_non_paddened_extent]};
    cudaMemcpy2D(hostPtr.get(), sizeof(value_type) * width, devPtr, pitch,
                 width * sizeof(value_type), height, cudaMemcpyDeviceToHost);

    {
        auto var = IO.DefineVariable<value_type>("cpu_pointer_without_padding", {width, height},
                                                 {0, 0}, {width, height});
        engine.Put(var, hostPtr.get());
    }

    engine.EndStep();
    engine.Close();
}