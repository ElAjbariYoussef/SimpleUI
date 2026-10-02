// Asset access layer. Everything the engine loads goes through here rather than
// direct file IO, so the packaged build can swap in a bundle that reads from
// inside the executable without touching callers.
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace sui {

class Resources {
public:
    Resources() = default;
    virtual ~Resources() = default;

    Resources(const Resources&) = delete;
    Resources& operator=(const Resources&) = delete;

    // Root-relative lookup: "/assets/style.css" -> "<root>/assets/style.css".
    virtual std::string resolve(std::string_view href) const = 0;

    // Returns nullopt when the asset is absent.
    virtual std::optional<std::string> readText(std::string_view href) const = 0;

    virtual bool exists(std::string_view href) const = 0;
};

// Development-time backend: assets live on disk next to the entry .sui file.
class FileResources final : public Resources {
public:
    explicit FileResources(std::string root);

    std::string resolve(std::string_view href) const override;
    std::optional<std::string> readText(std::string_view href) const override;
    bool exists(std::string_view href) const override;

    const std::string& root() const noexcept { return root_; }

private:
    std::string root_;
};

// Derives a resource root from the path of the entry .sui file.
std::string projectRootFor(std::string_view entrySuiPath);

}