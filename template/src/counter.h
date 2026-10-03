// State shared by the template's two scripts.
//
// Scripts are plain C++ translation units the build links together, so shared
// state is shared the normal C++ way: declare it in a header, define it in one
// .cpp.
#pragma once

int counter();

// Writes the new value into the <p id="counter"> element and marks the tree
// changed, so the next frame lays out and paints the new text.
void setCounter(int value);

// The onclick handler behind the button in index.sui.
void addone();