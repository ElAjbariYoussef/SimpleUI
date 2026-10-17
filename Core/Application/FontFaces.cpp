#include "Core/Application/FontFaces.h"

#include <string>

#include "CSS/Parser.h"
#include "Core/Resources/Resources.h"
#include "Rendering/Text.h"

namespace sui {

std::size_t loadFontFaces(TextFont& font, const Resources& resources, std::string_view css,
                          std::string_view sheetHref) {
    std::size_t added = 0;
    for (const FontFaceRule& rule : CSSParser::parseFontFaces(css, sheetHref)) {
        if (font.hasFace(rule.family, rule.weight, rule.italic)) {
            continue;
        }
        // Candidate sources are in author order and the first that resolves
        // and decodes wins; a candidate that is missing or is not a font
        // simply hands over to the next one.
        for (const std::string& src : rule.src) {
            auto data = resources.readText(src);
            if (!data) {
                continue;
            }
            if (font.addFaceFromMemory(rule.family,
                                       reinterpret_cast<const unsigned char*>(data->data()),
                                       static_cast<int>(data->size()), rule.weight,
                                       rule.italic)) {
                ++added;
                break;
            }
        }
    }
    return added;
}

}  // namespace sui
