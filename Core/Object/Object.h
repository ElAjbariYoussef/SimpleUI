// Base of the object system: every engine entity carries a type name so the
// registry and the packer can identify it without RTTI across DLL boundaries.
#pragma once

#include <string>
#include <string_view>

namespace sui {

class Object {
public:
    explicit Object(std::string type);
    virtual ~Object() = default;

    Object(const Object&) = delete;
    Object& operator=(const Object&) = delete;

    std::string_view type() const noexcept { return type_; }

private:
    std::string type_;
};

}