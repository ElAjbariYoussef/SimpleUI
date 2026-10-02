#include "Core/Resources/Resources.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace sui {
namespace {

namespace fs = std::filesystem;

}  // namespace

std::string projectRootFor(std::string_view entrySuiPath) {
    fs::path entry(entrySuiPath);
    fs::path dir = entry.parent_path();
    if (dir.empty()) {
        dir = fs::current_path();
    }
    std::error_code ec;
    fs::path canonical = fs::weakly_canonical(dir, ec);
    if (ec) {
        canonical = dir;
    }
    return canonical.string();
}

FileResources::FileResources(std::string root) : root_(std::move(root)) {}

std::string FileResources::resolve(std::string_view href) const {
    // A leading '/' is project-root-relative, not filesystem-root.
    fs::path relative(href);
    if (relative.is_absolute()) {
        relative = relative.relative_path();
    }
    return (fs::path(root_) / relative).lexically_normal().string();
}

std::optional<std::string> FileResources::readText(std::string_view href) const {
    const std::string path = resolve(href);
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

bool FileResources::exists(std::string_view href) const {
    std::error_code ec;
    return fs::exists(fs::path(resolve(href)), ec);
}

}  // namespace sui