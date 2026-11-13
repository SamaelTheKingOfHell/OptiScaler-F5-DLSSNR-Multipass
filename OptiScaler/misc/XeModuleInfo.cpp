#include "pch.h"
#include "XeModuleInfo.h"

#include <Config.h>
#include <Util.h>

#include "LibraryPathCheck.h"
#include <imgui/ImGuiNotify.hpp>

#include <proxies/KernelBase_Proxy.h>
#include <proxies/Ntdll_Proxy.h>
#include <proxies/XeFG_Proxy.h>
#include <proxies/XeLL_Proxy.h>

#include <atomic>
#include <fstream>

#include <bcrypt.h>
#include <SoftPub.h>
#include <WinTrust.h>

#pragma comment(lib, "bcrypt.lib")

namespace
{
std::mutex lineMutex;
std::string xefgLine;
std::string xellLine;

// The modules last described, and whether a description is under way (all under lineMutex)
HMODULE describedXefg = nullptr;
HMODULE describedXell = nullptr;
bool describing = false;

std::wstring PathOf(HMODULE module)
{
    if (module == nullptr)
        return {};

    wchar_t path[MAX_PATH] {};
    const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
    return length == 0 || length >= MAX_PATH ? std::wstring() : std::wstring(path, length);
}

bool SameFolder(const std::filesystem::path& file, const std::wstring& folder)
{
    std::error_code error;
    return !folder.empty() && std::filesystem::equivalent(file.parent_path(), folder, error);
}

// Where a module's file comes from, in the words the menu uses. Without an OptiScaler subfolder OptiScaler's folder
// is the game's own, and the label says so.
std::string SourceOf(const std::filesystem::path& file, const std::wstring& overridePath,
                     const std::wstring& optiFolder, const std::wstring& pluginFolder)
{
    std::error_code error;

    if (!overridePath.empty() &&
        (std::filesystem::equivalent(file, overridePath, error) || SameFolder(file, overridePath)))
        return "the ini's path";

    // FG Output F5Low's f5low.dll (and its Intel fallback) ship there
    if (SameFolder(file, pluginFolder))
        return "the plugins folder";

    const bool inGameFolder = SameFolder(file, Util::ExePath().parent_path().wstring());

    if (SameFolder(file, optiFolder))
        return inGameFolder ? "the game's folder (also OptiScaler's)" : "OptiScaler's folder";

    if (inGameFolder)
        return "the game's folder";

    return "elsewhere";
}

std::string VersionOf(const std::wstring& path)
{
    version_t version {};

    if (!Util::GetFileVersion(path, &version))
        return "no version";

    return std::format("v{}.{}.{}.{}", version.major, version.minor, version.patch, version.reserved);
}

// The file's Authenticode signature, checked offline (no revocation lookup)
std::string SignatureOf(const std::wstring& path)
{
    using PFN_WinVerifyTrust = LONG(WINAPI*)(HWND, GUID*, LPVOID);

    static const auto verify = []
    {
        const auto wintrust = NtdllProxy::LoadLibraryExW_Ldr(L"wintrust.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return wintrust != nullptr ? (PFN_WinVerifyTrust) KernelBaseProxy::GetProcAddress_()(wintrust, "WinVerifyTrust")
                                   : nullptr;
    }();

    if (verify == nullptr)
        return "signature not checked";

    WINTRUST_FILE_INFO file {};
    file.cbStruct = sizeof(file);
    file.pcwszFilePath = path.c_str();

    WINTRUST_DATA data {};
    data.cbStruct = sizeof(data);
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_NONE;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &file;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;

    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG result = verify(nullptr, &action, &data);

    data.dwStateAction = WTD_STATEACTION_CLOSE;
    verify(nullptr, &action, &data);

    switch (result)
    {
    case ERROR_SUCCESS:
        return "signed";

    case TRUST_E_NOSIGNATURE:
        return "not signed";

    case TRUST_E_BAD_DIGEST:
        return "changed after signing";

    default:
        return std::format("signature not trusted ({:#x})", (uint32_t) result);
    }
}

std::string Sha256Of(const std::wstring& path)
{
    std::ifstream file(path, std::ios::binary);
    BCRYPT_HASH_HANDLE hash = nullptr;

    if (!file || BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash, nullptr, 0, nullptr, 0, 0) != 0)
        return "unavailable";

    std::vector<char> chunk(1 << 20);
    bool ok = true;

    while (ok && file)
    {
        file.read(chunk.data(), (std::streamsize) chunk.size());

        if (file.gcount() > 0)
            ok = BCryptHashData(hash, (PUCHAR) chunk.data(), (ULONG) file.gcount(), 0) == 0;
    }

    unsigned char digest[32] {};
    ok = ok && BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0;
    BCryptDestroyHash(hash);

    if (!ok)
        return "unavailable";

    std::string hex;
    for (const auto byte : digest)
        hex += std::format("{:02x}", byte);

    return hex;
}

// One module (libxess_fg.dll or libxell.dll), as read on the calling thread
struct Module
{
    const char* what = nullptr;
    HMODULE module = nullptr;
    HMODULE gameCopy = nullptr;
    std::wstring path;
    std::wstring overridePath;
};

// The log line and the menu line of one module
std::string DescribeOne(const Module& m, const std::wstring& optiFolder, const std::wstring& pluginFolder)
{
    if (m.module == nullptr || m.path.empty())
    {
        LOG_INFO("{}: not loaded", m.what);
        return {};
    }

    const std::filesystem::path file(m.path);
    const auto source = SourceOf(file, m.overridePath, optiFolder, pluginFolder);
    const auto version = VersionOf(m.path);
    const auto signature = SignatureOf(m.path);

    LOG_INFO("{} in use: {} (from {}), file {}, {}, SHA-256 {}", m.what, wstring_to_string(m.path), source, version,
             signature, Sha256Of(m.path));

    std::string line = std::format("{}: {} {} from {}, {}", m.what, wstring_to_string(file.filename().wstring()),
                                   version, source, signature);

    if (m.gameCopy != nullptr && m.gameCopy != m.module)
    {
        const auto gamePath = PathOf(m.gameCopy);
        const auto gameVersion = VersionOf(gamePath);

        LOG_INFO("{}: the game's own copy is loaded too: {}, file {}", m.what, wstring_to_string(gamePath),
                 gameVersion);
        line += std::format("; the game's own {} is loaded too", gameVersion);
    }

    return line;
}

struct Work
{
    Module xefg;
    Module xell;
    std::wstring optiFolder;
    std::wstring pluginFolder;
    HMODULE self = nullptr; // a reference on OptiScaler, so it stays mapped while the thread runs
};

DWORD WINAPI DescribeThread(LPVOID parameter)
{
    std::unique_ptr<Work> work(static_cast<Work*>(parameter));
    const HMODULE self = work->self;

    try
    {
        auto fg = DescribeOne(work->xefg, work->optiFolder, work->pluginFolder);
        auto ll = DescribeOne(work->xell, work->optiFolder, work->pluginFolder);

        std::scoped_lock lock(lineMutex);
        xefgLine = std::move(fg);
        xellLine = std::move(ll);
    }
    catch (...)
    {
        LOG_ERROR("Describing XeFG / XeLL failed");
    }

    {
        std::scoped_lock lock(lineMutex);
        describing = false;
    }

    work.reset();
    FreeLibraryAndExitThread(self, 0);
}
} // namespace

void XeModuleInfo::Describe()
{
    const HMODULE xefg = XeFGProxy::Module();
    const HMODULE xell = XeLLProxy::Module();

    {
        std::scoped_lock lock(lineMutex);

        // Once per pair of modules: again only when one loaded (or changed) since
        if (describing || (xefg == describedXefg && xell == describedXell))
            return;

        describing = true;
        describedXefg = xefg;
        describedXell = xell;
    }

    // Read here: the paths and handles do not change once the proxies have loaded
    auto work = std::make_unique<Work>();
    // F5Low does not use XeFGPath (it loads from the plugins folder), so the ini's path is not its source
    const bool f5low = State::Instance().activeFgOutput == FGOutput::F5Low;
    work->xefg = { "XeFG", xefg, XeFGProxy::MemModule(), XeFGProxy::Module_Path(),
                   f5low ? std::wstring() : Config::Instance()->XeFGLibrary.value_or(L"") };
    work->xell = { "XeLL", xell, XeLLProxy::MemModule(), XeLLProxy::Module_Path(),
                   Config::Instance()->XeLLLibrary.value_or(L"") };
    work->optiFolder = Config::Instance()->MainDllPath.value_or(L"");
    work->pluginFolder = Config::Instance()->PluginPath.value_or(L"");

    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCWSTR) &DescribeThread, &work->self))
        work->self = nullptr;

    if (work->self != nullptr)
    {
        if (const auto thread = CreateThread(nullptr, 0, DescribeThread, work.get(), 0, nullptr))
        {
            work.release(); // the thread owns it now
            CloseHandle(thread);
            return;
        }

        FreeLibrary(work->self);
    }

    std::scoped_lock lock(lineMutex);
    describing = false;
}

std::string XeModuleInfo::XeFGLine()
{
    Describe(); // picks up an XeFG or XeLL loaded after start

    std::scoped_lock lock(lineMutex);
    return xefgLine;
}

void XeModuleInfo::CheckXeFGPath()
{
    static std::atomic<bool> checked = false;

    if (checked.exchange(true))
        return;

    const auto verdict = LibraryPathCheck::Check(
        "XeFG", L"libxess_fg.dll", Config::Instance()->XeFGLibrary.value_or(L""), XeFGProxy::Module_Path());

    if (verdict.result != LibraryPathCheck::Result::NotFound && verdict.result != LibraryPathCheck::Result::NotUsed)
        return;

    LOG_WARN("{}", verdict.message);

    ImGuiToast toast { ImGuiToastType::Warning, 30000 };
    toast.setTitle("XeFGPath is not used");
    toast.setContent("%s", verdict.message.c_str());
    ImGui::InsertNotification(toast);
}

std::string XeModuleInfo::XeLLLine()
{
    std::scoped_lock lock(lineMutex);
    return xellLine;
}
