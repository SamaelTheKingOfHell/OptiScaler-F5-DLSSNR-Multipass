#include "pch.h"
#include "../../OptiScaler/dlssnr/DlssNr_WorkerFenceWait.h"
#include "nr_cross_adapter_test_helpers.h"
#include <cwchar>

using DlssNr::MultiGpu::WaitForFenceOrPeer;
using DlssNr::MultiGpu::WorkerFenceWaitResult;

static void Expect(WorkerFenceWaitResult actual, WorkerFenceWaitResult expected, const char* detail)
{
    if (actual != expected)
        throw std::runtime_error(std::string(detail) + " (actual " + std::to_string(static_cast<int>(actual)) + ")");
}
int wmain(int argc, wchar_t** argv)
{
    try
    {
        if (argc == 2 && std::wcscmp(argv[1], L"--exit") == 0)
            return 0;
        if (argc != 1)
            return 2;
        ComPtr<IDXGIFactory4> factory;
        Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "Create factory");
        ComPtr<IDXGIAdapter1> adapter;
        Check(factory->EnumAdapters1(0, &adapter), "Select adapter");
        ComPtr<ID3D12Device> device;
        Check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "Create device");
        ComPtr<ID3D12Fence> fence;
        Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "Create fence");
        HANDLE realSelf = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &realSelf, SYNCHRONIZE,
                             FALSE, 0))
            throw std::runtime_error("Duplicate self handle failed");
        Expect(WaitForFenceOrPeer(nullptr, 1, realSelf, 0), WorkerFenceWaitResult::Failed, "Accepted null fence");
        Expect(WaitForFenceOrPeer(fence.Get(), 0, realSelf, 0), WorkerFenceWaitResult::Failed, "Accepted zero ticket");
        Expect(WaitForFenceOrPeer(fence.Get(), 1, realSelf, INFINITE), WorkerFenceWaitResult::Failed,
               "Accepted infinite wait");
        Expect(WaitForFenceOrPeer(fence.Get(), 1, realSelf, 1), WorkerFenceWaitResult::Timeout,
               "Unsignalled fence did not time out");
        Check(fence->Signal(1), "Signal fence");
        Expect(WaitForFenceOrPeer(fence.Get(), 1, realSelf, 1), WorkerFenceWaitResult::Ready,
               "Signalled fence was not ready");
        CloseHandle(realSelf);
        wchar_t path[32768] {};
        DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
        if (!length || length >= std::size(path))
            throw std::runtime_error("Executable path unavailable");
        std::wstring command = L"\"" + std::wstring(path) + L"\" --exit";
        STARTUPINFOW startup {};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION child {};
        if (!CreateProcessW(path, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                            &child))
            throw std::runtime_error("Could not start peer process");
        CloseHandle(child.hThread);
        const auto result = WaitForFenceOrPeer(fence.Get(), 2, child.hProcess, 30000);
        const DWORD waited = WaitForSingleObject(child.hProcess, 30000);
        CloseHandle(child.hProcess);
        Expect(result, WorkerFenceWaitResult::PeerExited, "Peer exit did not interrupt unsignalled fence wait");
        if (waited != WAIT_OBJECT_0)
            throw std::runtime_error("Peer did not exit");
        std::cout << "Fence readiness, timeout and real peer-process exit passed without a GPU queue wait.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << "\n";
        return 1;
    }
}
