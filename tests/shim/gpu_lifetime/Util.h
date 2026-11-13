#pragma once

// Stand-in for OptiScaler's Util.h (tests/nr_gpu_lifetime_gpu.cpp): no Streamline proxies here, an object is its own
// identity.
#include <unknwn.h>

namespace Util
{
inline IUnknown* RealObjectIdentity(IUnknown* object) { return object; }
} // namespace Util
