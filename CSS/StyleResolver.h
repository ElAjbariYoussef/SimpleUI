#pragma once

#include "CSS/Animation.h"
#include "CSS/DefaultStyle.h"
#include "CSS/Parser.h"
#include "CSS/Style.h"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace sui {

class Element;

// Which elements are currently hovered, pressed, and focused. A selector's
// `:hover`/`:active`/`:focus` parts are matched against this, so passing a null
// pointer is the same as saying nothing is in any state.
struct PseudoState {
    const Element* hovered = nullptr;
    const Element* active = nullptr;
    const Element* focused = nullptr;
    // Whether the focused element wants visible focus UI: set on keyboard
    // interaction, or on focusing a control that takes text entry.
    bool focusVisible = false;
};

// Per-element animation state, kept between resolves. `target` is the previous
// raw computed style (before interpolation): a transition compares author values
// against it, so an interpolated output can never restart its own transition.
// Runs live here, not in ComputedStyle, because resolution itself must stay
// const and the runs are pure runtime bookkeeping.
struct ElementAnim {
    ComputedStyle target;
    bool hasTarget = false;

    // One property moving from a start value to a target over a delay.
    struct Transition {
        std::string property;
        double startMs;  // clock time the run began
        double durationMs;
        double delayMs;
        AnimatableValue from;
        AnimatableValue to;
        Easing easing;
    };
    std::vector<Transition> transitions;

    // One named @keyframes animation playing on the element.
    struct KeyframeRun {
        std::string name;
        double startMs;
        double durationMs;
        double delayMs;
        float iterations;  // < 0 = infinite
        std::string direction;  // normal/reverse/alternate/alternate-reverse
        std::string fill;       // none/forwards/backwards/both
        bool paused = false;
        Easing easing;
    };
    std::vector<KeyframeRun> keyframeRuns;
};

// Resolves the computed style for an element: user-agent defaults, then
// matching stylesheet rules by specificity, then the inline style attribute.
class StyleResolver {
public:
    ComputedStyle resolve(Element& element) const;

    void setRules(std::vector<Rule> rules);
    void addRule(Rule rule);
    void eraseRule(std::size_t index);
    void clearRules() {
        rules_.clear();
        ++generation_;
    }

    const std::vector<Rule>& rules() const noexcept { return rules_; }
    bool empty() const noexcept { return rules_.empty(); }

    // Registers `@keyframes` blocks. A later block with the same name replaces
    // the earlier one, like a second stylesheet winning a name collision.
    void setKeyframes(std::vector<KeyframeBlock> keyframes);
    void addKeyframes(std::vector<KeyframeBlock> keyframes);

    // The clock transitions and keyframe animations tick against. Defaults to
    // the script surface's monotonic clock; tests replace it so a frame can be
    // stepped deterministically. The function stays owned by the caller and must
    // outlive the resolver.
    void setTimeSource(double (*timeSource)()) noexcept { timeSource_ = timeSource; }

    // True while any element has a transition or animation in its moving span
    // (the delay is counted too: the from-value is on screen and must stay
    // there, and repainting through a short delay is cheaper than a scheduler
    // hook). The frame loop folds this into its stale set, so a moving page
    // keeps repainting without any script asking.
    bool hasRunningAnimations() const;

    // Serialises one property of a resolved style back to CSS text, so a script
    // can read what it sees. Returns an empty string for an unsupported name.
    static std::string propertyValue(const ComputedStyle& style, std::string_view property);

    // The rule-ordering weight of a selector: ids * 10000 + classes * 100 + types,
    // with attribute selectors counted as classes and pseudo-elements as types.
    static int specificityOf(const SimpleSelector& selector);

    // Bumped by every change to the rule set, so the frame loop can notice that
    // resolution may differ even when the DOM did not move.
    std::uint64_t generation() const noexcept { return generation_; }

    // Whether any registered rule sits under `@container`. Layout asks once
    // per pass: without container rules there is nothing to re-read.
    bool anyContainerRules() const noexcept { return anyContainerRules_; }

    // What `rem` and initial `font-size: <percentage>` resolve against. The
    // application sets this from the document root once it is known.
    float rootFontSize() const noexcept { return rootFontSize_; }
    void setRootFontSize(float size);

    // The viewport every `@media` is evaluated against, in CSS pixels. It starts
    // at 0x0, so a rule under `max-width` matches until the application reports a
    // size — which is why the application has to set this before the first
    // resolve. Changing it bumps the generation, so the frame loop re-resolves
    // and a resize switches breakpoints the same way the initial size does.
    bool setViewportSize(float width, float height);
    float viewportWidth() const noexcept { return viewportWidth_; }
    float viewportHeight() const noexcept { return viewportHeight_; }

    // Interactive state the pseudo-classes are matched against. Each setter
    // returns whether anything changed, so the caller only re-lays-out on a real
    // transition rather than on every mouse move.
    bool setHovered(const Element* element);
    bool setActive(const Element* element);
    bool setFocused(const Element* element);
    bool setFocusVisible(bool visible);
    bool focusVisible() const noexcept { return state_.focusVisible; }
    const PseudoState& pseudoState() const noexcept { return state_; }

private:
    ComputedStyle resolveWithParent(Element& element, const ComputedStyle& parent) const;

    // Cascade layers (`@layer`), in first-declared order. A rule's layer sorts
    // before its specificity: later layers beat earlier ones, and unlayered
    // rules beat every layer (reversed for `!important`).
    void noteLayer(const std::string& name);
    int layerIndex(const std::string& name) const;
    int layerRank(const std::string& layer, bool important) const;
    void sortRulesNormal();
    void rebuildImportantOrder();
    // Whether a rule under `@container` reads the nearest query container
    // ancestor's laid-out content width. Const because `resolve()` is.
    bool containerMatches(const Rule::ContainerQuery& query, Element& element) const;

    // Runs every transition and keyframe animation for `element` into `style`,
    // which must still hold the raw computed values. Called at the end of
    // resolveWithParent so layout and paint always see the interpolated picture.
    void animateStyle(Element& element, ComputedStyle& style) const;
    void reconcileKeyframes(ComputedStyle& style, ElementAnim& anim, double now) const;
    void applyKeyframes(ComputedStyle& style, ElementAnim& anim, double now) const;
    double currentTimeMs() const;

    // Kept sorted weakest-first so resolve() can apply rules in order.
    std::vector<Rule> rules_;
    // The same rules weakest-first for `!important`, which reverses layer
    // order: unlayered first, then layers latest-declared first. Stored as
    // indices because erasing reorders nothing else.
    std::vector<std::size_t> importantOrder_;
    // `@layer` names in first-declared order, from statements and first use.
    std::vector<std::string> layerOrder_;
    bool anyContainerRules_ = false;
    std::map<std::string, std::vector<KeyframeBlock::Keyframe>, std::less<>> keyframes_;
    mutable std::map<const Element*, ElementAnim> animations_;
    mutable double (*timeSource_)() = nullptr;
    std::uint64_t generation_ = 0;
    float rootFontSize_ = 16.0f;
    float viewportWidth_ = 0.0f;
    float viewportHeight_ = 0.0f;
    PseudoState state_;
};

// True when `selector` matches `element`, treating a compound selector as
// requiring all of its simple parts on the same element. Pass `state` to
// evaluate `:hover`, `:active`, and `:focus`.
bool selectorMatches(const SimpleSelector& selector, const Element& element,
                     const PseudoState* state = nullptr);

// Parses a CSS `<color>` token -- hex, named, rgb()/rgba(), hsl(), oklch(),
// color-mix() -- into an sRGB color. Returns false for anything else. Used by
// the resolver for colour properties and by the `filter:` parser for
// `drop-shadow()`'s colour argument.
bool parseColor(std::string_view value, SDL_Color& out);

}  // namespace sui
