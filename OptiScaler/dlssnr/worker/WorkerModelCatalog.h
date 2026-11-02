#pragma once

#include <algorithm>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace DlssNr::Worker
{
// User-supplied runtimes live below each host root. Labels never become paths.
// ```cpp
// auto models = ModelCatalog::Discover({ dllDirectory, executableDirectory });
// auto model = ModelCatalog::Resolve({ dllDirectory }, "my_runtime");
// ```
class ModelCatalog
{
    using Path = std::filesystem::path;
    using Entries = std::map<std::string, std::pair<std::string, Path>>;

    static std::string Fold(std::string value)
    {
        for (auto& c : value)
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        return value;
    }
    static bool Valid(const std::string& id)
    {
        if (id.empty() || id.size() > 64 || Fold(id) == "auto") return false;
        return std::all_of(id.begin(), id.end(), [](char c)
        {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '_' || c == '-';
        });
    }
    static std::optional<std::string> Label(const Path& directory)
    {
        std::string label;
        const auto native = directory.filename().native();
        for (const auto c : native)
        {
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-')) return std::nullopt;
            label.push_back(static_cast<char>(c));
        }
        return Valid(label) ? std::optional<std::string>(label) : std::nullopt;
    }
    static Entries ReadRoot(const Path& root)
    {
        Entries entries;
        if (root.empty()) return entries;
        std::error_code error;
        const auto directory = root / "OptiScaler" / "NR-Models";
        std::filesystem::directory_iterator current(directory, std::filesystem::directory_options::skip_permission_denied,
                                                     error), end;
        while (!error && current != end)
        {
            const auto& entry = *current;
            std::error_code statusError;
            auto label = Label(entry.path());
            if (label && entry.is_directory(statusError) && !statusError)
            {
                const auto model = entry.path() / "nvngx_dlssnr.dll";
                statusError.clear();
                // Follow user-owned links, but only inspect this fixed filename.
                if (std::filesystem::is_regular_file(model, statusError) && !statusError)
                {
                    const auto key = Fold(*label);
                    auto [position, inserted] = entries.emplace(key, std::make_pair(*label, model));
                    // Case-sensitive filesystems can contain two case variants.
                    if (!inserted && *label < position->second.first)
                        position->second = { *label, model };
                }
            }
            current.increment(error);
        }
        return entries;
    }

  public:
    // Return stable case-insensitive IDs; the first root supplies duplicate labels.
    // ```cpp
    // for (const auto& id : ModelCatalog::Discover(roots)) ShowModel(id);
    // ```
    static std::vector<std::string> Discover(const std::vector<Path>& roots)
    {
        Entries entries;
        for (const auto& root : roots)
            for (const auto& [key, value] : ReadRoot(root)) entries.emplace(key, value);
        std::vector<std::string> ids;
        ids.reserve(entries.size());
        for (const auto& [key, value] : entries) ids.push_back(value.first);
        return ids;
    }

    // Resolve only a catalog label. Empty/auto remains the host's fallback choice.
    // ```cpp
    // if (auto path = ModelCatalog::Resolve(roots, selectedId)) LoadModel(*path);
    // ```
    static std::optional<Path> Resolve(const std::vector<Path>& roots, const std::string& id)
    {
        if (!Valid(id)) return std::nullopt;
        const auto key = Fold(id);
        for (const auto& root : roots)
        {
            const auto entries = ReadRoot(root);
            const auto found = entries.find(key);
            if (found != entries.end()) return found->second.second;
        }
        return std::nullopt;
    }
};
} // namespace DlssNr::Worker
