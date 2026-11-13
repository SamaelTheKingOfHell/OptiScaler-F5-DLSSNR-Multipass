#pragma once

// Stand-in for OptiScaler's precompiled header, so tests/nr_gpu_lifetime_gpu.cpp builds dlssnr/DlssNr_GpuLifetime.cpp
// alone: only what that file takes from it.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>

#include <functional>
#include <memory>
