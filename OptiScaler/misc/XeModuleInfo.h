#pragma once

#include <string>

// Which XeFG and XeLL files are in use, for the log and the menu: the path and where it comes from (OptiScaler's
// folder, the game's, the override in the ini), the file version, whether its signature checks out, its SHA-256, and a
// second copy of the game's own when one is loaded too. Worked out on a background thread (hashing and the signature
// check read the whole file); until then the menu lines are empty.
namespace XeModuleInfo
{
// Starts the background work when XeFGProxy / XeLLProxy hold modules not described yet; otherwise does nothing.
// Cheap enough to call every frame.
void Describe();

// One short line for the menu, empty while it is being worked out or when that module is not loaded. XeFGLine also
// calls Describe, so a module loaded after start is picked up while the menu shows.
std::string XeFGLine();
std::string XeLLLine();

// Once per run, after the first attempt to load libxess_fg.dll: when the ini's XeFGPath names a file that is missing or
// was not the one loaded, says so in the log and with a toast (the loader quietly uses another copy otherwise).
void CheckXeFGPath();
} // namespace XeModuleInfo
