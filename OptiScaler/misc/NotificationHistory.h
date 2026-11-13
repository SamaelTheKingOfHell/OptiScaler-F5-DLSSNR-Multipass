#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// Every toast the overlay showed, kept for the menu's Notifications page. Plain data and no ImGui, so it can be unit
// tested on the host. ImGui::InsertNotification (include/imgui/ImGuiNotify.hpp) records into it; toasts come from many
// threads, so everything here takes the lock.
namespace NotificationHistory
{
// The oldest entries are dropped past this many.
inline constexpr size_t kMaxEntries = 200;

struct Entry
{
    uint64_t id = 0;  // grows with every new entry, so the page can tell what it has seen
    uint8_t type = 0; // ImGuiToastType, kept as a number
    std::string title;
    std::string content;
    uint32_t count = 1;                          // a toast repeated back to back is one entry shown "x N"
    std::chrono::system_clock::time_point first; // when it first appeared
    std::chrono::system_clock::time_point last;  // when it last appeared
};

namespace detail
{
struct Store
{
    std::mutex mutex;
    std::vector<Entry> entries; // oldest first
    uint64_t nextId = 1;
    uint64_t total = 0; // every toast shown, repeats included, not reduced by Clear
};

inline Store& Instance()
{
    static Store store;
    return store;
}
} // namespace detail

// Notes one toast. The same toast as the newest entry only raises that entry's count.
inline void Add(uint8_t type, const char* title, const char* content,
                std::chrono::system_clock::time_point now = std::chrono::system_clock::now())
{
    auto& store = detail::Instance();
    std::scoped_lock lock(store.mutex);

    const std::string t = title != nullptr ? title : "";
    const std::string c = content != nullptr ? content : "";

    ++store.total;

    if (!store.entries.empty())
    {
        auto& newest = store.entries.back();

        if (newest.type == type && newest.title == t && newest.content == c)
        {
            ++newest.count;
            newest.last = now;
            return;
        }
    }

    Entry entry;
    entry.id = store.nextId++;
    entry.type = type;
    entry.title = t;
    entry.content = c;
    entry.first = now;
    entry.last = now;
    store.entries.push_back(std::move(entry));

    if (store.entries.size() > kMaxEntries)
        store.entries.erase(store.entries.begin(), store.entries.begin() + (store.entries.size() - kMaxEntries));
}

// A copy, oldest first: the page draws from it while other threads keep adding.
inline std::vector<Entry> Snapshot()
{
    auto& store = detail::Instance();
    std::scoped_lock lock(store.mutex);
    return store.entries;
}

// How many toasts were shown since start, repeats counted.
inline uint64_t Total()
{
    auto& store = detail::Instance();
    std::scoped_lock lock(store.mutex);
    return store.total;
}

inline void Clear()
{
    auto& store = detail::Instance();
    std::scoped_lock lock(store.mutex);
    store.entries.clear();
}
} // namespace NotificationHistory
