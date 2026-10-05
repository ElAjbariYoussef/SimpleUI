#pragma once

#include "SUI/DOM/Document.h"

namespace sui {

class StyleResolver;

// Rebuilds the `::before` and `::after` boxes the stylesheet generates, so a rule
// can add text to an element without markup for it. Run it after the resolver has
// its rules and before layout; it replaces the boxes it made last time rather than
// adding to them, so it is safe to call on every pass.
//
// A pseudo-element with an empty `content` produces no box, which is also what
// `content: none` means.
void generatePseudoElements(Document& document, const StyleResolver& resolver);

}  // namespace sui