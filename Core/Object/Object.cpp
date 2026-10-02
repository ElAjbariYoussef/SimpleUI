#include "Core/Object/Object.h"

#include <utility>

namespace sui {

Object::Object(std::string type) : type_(std::move(type)) {}

}