// Filesystem-only catalog checks. These inert files are never loaded as DLLs.
// cl /std:c++20 /EHsc /W4 tests/nr_worker_model_catalog_smoke.cpp
#include "../OptiScaler/dlssnr/worker/WorkerModelCatalog.h"

#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>

using DlssNr::Worker::ModelCatalog;
namespace fs = std::filesystem;

static void Check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

class TempDirectory
{
    fs::path parent_;
  public:
    fs::path path;
    TempDirectory() : parent_(fs::temp_directory_path())
    {
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned attempt = 0; attempt < 100; ++attempt)
        {
            path = parent_ / ("nr-model-catalog-" + std::to_string(unique) + "-" + std::to_string(attempt));
            if (fs::create_directory(path)) return;
        }
        throw std::runtime_error("Could not create isolated test directory");
    }
    ~TempDirectory()
    {
        // Delete only the directory created above, never a computed external target.
        if (path.parent_path() == parent_ && path.filename().string().rfind("nr-model-catalog-", 0) == 0)
        {
            std::error_code error;
            fs::remove_all(path, error);
        }
    }
};

static fs::path Models(const fs::path& root) { return root / "OptiScaler" / "NR-Models"; }
static fs::path AddModel(const fs::path& root, const std::string& id)
{
    const auto dll = Models(root) / id / "nvngx_dlssnr.dll";
    fs::create_directories(dll.parent_path());
    std::ofstream file(dll, std::ios::binary);
    Check(file.good(), "Could not create catalog entry");
    return dll;
}

int main()
{
    try
    {
        TempDirectory temporary;
        const auto first = temporary.path / "first", second = temporary.path / "second";
        const auto missing = temporary.path / "missing";
        Check(ModelCatalog::Discover({ missing }).empty(), "Missing root was not empty");
        Check(!ModelCatalog::Resolve({ missing }, "missing"), "Missing entry resolved");

        const auto zulu = AddModel(first, "Zulu");
        const auto sharedFirst = AddModel(first, "Shared_Model-2");
        const auto sharedSecond = AddModel(second, "shared_model-2");
        const auto alpha = AddModel(second, "alpha");
        AddModel(first, "bad.name");
        AddModel(first, "auto");
        AddModel(first, std::string(65, 'x'));
        fs::create_directories(Models(first) / "MissingDll");
        fs::create_directories(Models(first) / "DirectoryDll" / "nvngx_dlssnr.dll");
        std::ofstream flat(Models(first) / "FlatFile");
        flat.close();
        const auto fileRoot = temporary.path / "file-root";
        std::ofstream invalidRoot(fileRoot);
        invalidRoot.close();
        const std::vector<fs::path> roots { missing, fileRoot, first, second, first };
        Check(ModelCatalog::Discover(roots) == std::vector<std::string>({ "alpha", "Shared_Model-2", "Zulu" }),
              "Catalog order, validation, or duplicate precedence failed");
        Check(ModelCatalog::Resolve(roots, "ZULU") == zulu, "Case-insensitive lookup failed");
        Check(ModelCatalog::Resolve(roots, "shared_MODEL-2") == sharedFirst, "First-root precedence failed");
        Check(ModelCatalog::Resolve({ second, first }, "shared_MODEL-2") == sharedSecond,
              "Reversed-root precedence failed");
        Check(ModelCatalog::Resolve(roots, "alpha") == alpha, "Second-root entry missing");

        for (const std::string id : { "", "auto", "AUTO", ".", "..", "../Zulu", "..\\Zulu", "/Zulu",
                                      "C:\\Zulu", "space id", "bad.name", "Zulu/child", "Zulu\\child" })
            Check(!ModelCatalog::Resolve(roots, id), "Unsafe or reserved label resolved");
        Check(!ModelCatalog::Resolve(roots, std::string("Zulu\0child", 10)), "Embedded null label resolved");
        Check(!ModelCatalog::Resolve(roots, std::string(65, 'x')), "Oversized label resolved");
        const std::string longest(64, 'x');
        const auto longPath = AddModel(second, longest);
        Check(ModelCatalog::Resolve(roots, longest) == longPath, "64-character label rejected");

        // User-owned directory links are allowed; lookup still uses one safe label.
        std::error_code linkError;
        fs::create_directory_symlink(alpha.parent_path(), Models(first) / "Linked", linkError);
        if (!linkError)
            Check(ModelCatalog::Resolve(roots, "linked") == Models(first) / "Linked" / "nvngx_dlssnr.dll",
                  "User-owned directory link failed");
        else
            std::cout << "Directory-link check skipped: " << linkError.message() << '\n';

        fs::remove(sharedFirst);
        Check(ModelCatalog::Resolve(roots, "Shared_Model-2") == sharedSecond,
              "Missing first-root DLL did not permit second-root resolution");
        Check(!ModelCatalog::Resolve(roots, "DirectoryDll"), "Directory was accepted as a DLL");
        std::cout << "Worker model catalog filesystem checks passed.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
