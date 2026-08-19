#include "ui/bottom.h"

#include "ui/hotbar.h"
#include "ui/vitals.h"

#include <algorithm>
#include <cmath>

bottom::Strip bottom::Of() {
    Strip strip;

    strip.bar = hotbar::Bounds();

    // Everything is stacked upwards off the bar, so a change to the bar's own size or
    // margin carries the rest with it and nothing has to be found and corrected.
    const float gutter = std::floor(strip.bar.y - kRowGap - kRowTall);

    // What the hearts need, and the rest for the hand. Bounded by the bar so that a
    // row of hearts can never grow past it — a strip wider than the thing it is
    // labelling stops reading as one block.
    const float taken = std::min(std::floor(vitals::kRowWide), strip.bar.width);

    strip.vitals = {strip.bar.x, gutter, taken, kRowTall};
    strip.hand   = {strip.bar.x + taken + kRowGap, gutter, std::max(0.0f, strip.bar.width - taken - kRowGap),
                    kRowTall};

    strip.name = {strip.bar.x, std::floor(gutter - kRowGap - kRowTall), strip.bar.width, kRowTall};

    return strip;
}
