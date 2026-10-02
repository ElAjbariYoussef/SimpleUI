#include "Platform/Platform.h"

#include "Platform/Linux/LinuxPlatform.h"

namespace sui {

// Only Linux exists today; the header-per-OS layout is what Platform/Windows and
// Platform/macOS will follow.
std::unique_ptr<Platform> makePlatform() {
    return std::make_unique<LinuxPlatform>();
}

}  // namespace sui