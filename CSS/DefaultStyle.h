// Default style lookup by element tag. Provides UA-like defaults.
#pragma once

#include "CSS/Style.h"

#include <string_view>

namespace sui {

ComputedStyle defaultStyleFor(std::string_view tag);

}  // namespace sui