#include "ui/pack.h"

#include "core/picture.h"
#include "ui/hotbar.h"

#include <algorithm>
#include <cmath>

namespace {

// Margin between the two panels, and between the pair and the edge of the frame.
constexpr float kBetween = 16.0f;
constexpr float kEdge    = 12.0f;

// The strip along the top of the chest holding the field and the buttons, as a
// multiple of one slot. One slot tall, so a button and a slot are the same height and
// the header reads as a row of the grid rather than as a bar bolted onto it.
constexpr float kHeadRows = 1.0f;

float PanelWide(const pack::Metrics &m) {
    return Inventory::kColumns * m.side + (Inventory::kColumns + 1) * m.pad;
}

float PanelTall(const pack::Metrics &m) {
    // Each grid row costs a slot and the padding after it, which is what `Slot` steps
    // by — counting the padding between rows instead leaves the total one short, and
    // the bar row comes out flush with the bottom edge with its counts running into
    // the border.
    return m.pad + Inventory::kRows * (m.side + m.pad) + m.gap + m.side + m.pad;
}

float StoreWide(const pack::Metrics &m) {
    return pack::kStoreColumns * m.side + (pack::kStoreColumns + 1) * m.pad;
}

float StoreTall(const pack::Metrics &m, int rows) {
    return m.pad + kHeadRows * m.side + m.pad + rows * (m.side + m.pad);
}

// How wide a button is. Proportional rather than measured, so the layout stays a pure
// function of the window and does not depend on a font being loaded.
float ButtonWide(const pack::Metrics &m) {
    return std::floor(m.side * 1.5f);
}

// The line of words under the store, and the air over it.
float HintTall(const pack::Metrics &m) {
    return static_cast<float>(m.font) + 8.0f;
}

// What the pair comes to at one texel, so the fit can be tried before it is committed
// to.
Vector2 PairSize(const pack::Metrics &m, int rows) {
    if (rows <= 0) return {PanelWide(m), PanelTall(m)};

    // The rules column and the hint under the grid are both reserved always — see
    // `Layout::Row` and `Layout::hint`.
    const float wide = PanelWide(m) + kBetween + StoreWide(m) + m.pad + m.side;
    const float tall = std::max(PanelTall(m), StoreTall(m, rows) + HintTall(m));

    return {wide, tall};
}

} // namespace

pack::Metrics pack::Metrics::At(float pixel) {
    Metrics m;

    m.pixel = pixel;

    // A slot is its picture plus a rim, and at six that comes to exactly the bar's own
    // forty-four. Written as the arithmetic rather than as a table of four sizes,
    // because what has to hold is that the picture inside is a whole number of pixels
    // and the slot is drawn round it.
    m.side = pixel * kPictureSide + 8.0f;
    m.pad  = pixel;
    m.gap  = 2.0f * pixel + 2.0f;
    m.font = std::max(8, static_cast<int>(std::lround(14.0f * pixel / 6.0f)));

    return m;
}

pack::Layout pack::Of(int storeRows) {
    const float wide = static_cast<float>(GetScreenWidth());
    const float tall = static_cast<float>(GetScreenHeight());

    // The largest texel the pair fits the frame at, walked down from the bar's own.
    //
    // Down rather than up, so the common case — a pack opened with no chest in front of
    // it — takes the first answer and is laid out exactly as it always was.
    Metrics metric = Metrics::At(static_cast<float>(kCoarsePixel));

    for (int pixel = kCoarsePixel; pixel >= kFinestPixel; pixel--) {
        metric = Metrics::At(static_cast<float>(pixel));

        const Vector2 pair = PairSize(metric, storeRows);

        if (pair.x <= wide - 2.0f * kEdge && pair.y <= tall - 2.0f * kEdge) break;
    }

    Layout at;

    at.metric    = metric;
    at.storeRows = std::max(storeRows, 0);

    const Vector2 pair = PairSize(metric, at.storeRows);

    const float left = std::floor((wide - pair.x) / 2.0f);
    const float top  = std::floor((tall - pair.y) / 2.0f);

    const float panelTall = PanelTall(metric);

    // Each panel centred against the taller of the two, so the pair reads as one thing
    // rather than as two panels sharing a top edge.
    at.panel = {left, std::floor(top + (pair.y - panelTall) / 2.0f), PanelWide(metric), panelTall};

    // The bin above the panel's right shoulder, outside it. Sat on the corner rather
    // than centred over the top edge, because the tabs of the creative palette hang
    // off the left of that same edge and two things claiming one strip is exactly the
    // fault this file exists to stop.
    at.trash = {at.panel.x + at.panel.width - metric.side, at.panel.y - metric.side - 4.0f, metric.side, metric.side};

    if (at.storeRows <= 0) return at;

    const float storeTall = StoreTall(metric, at.storeRows);
    const float withHint  = storeTall + HintTall(metric);

    // The grid and its caption centred as one thing, with the grid at the top of it, so
    // the words are inside what the pair reserved rather than hanging off the bottom.
    at.store = {at.panel.x + at.panel.width + kBetween, std::floor(top + (pair.y - withHint) / 2.0f),
                StoreWide(metric), storeTall};

    at.hint = {at.store.x, at.store.y + storeTall + 4.0f, at.store.width + metric.pad + metric.side,
               static_cast<float>(metric.font)};

    // The header: three buttons hard against the right edge, and the field taking
    // whatever is left. Measured off the right rather than laid out left to right, so
    // the buttons stay put as the panel narrows and it is the field that gives.
    const float button = ButtonWide(metric);
    const float head   = kHeadRows * metric.side;

    const float headY = at.store.y + metric.pad;
    float edge        = at.store.x + at.store.width - metric.pad;

    at.rules = {edge - button, headY, button, head};
    edge     = at.rules.x - metric.pad;

    at.sort = {edge - button, headY, button, head};
    edge    = at.sort.x - metric.pad;

    at.find = {edge - button, headY, button, head};
    edge    = at.find.x - metric.pad;

    at.search = {at.store.x + metric.pad, headY, std::max(edge - (at.store.x + metric.pad), metric.side), head};

    return at;
}

Rectangle pack::Layout::Slot(int slot) const {
    const int column = Inventory::OnHand(slot) ? slot : (slot - Inventory::kOnHand) % Inventory::kColumns;
    const int row    = Inventory::OnHand(slot) ? Inventory::kRows : (slot - Inventory::kOnHand) / Inventory::kColumns;

    // The bar row is one row further down plus the gap, which is what sets it apart as
    // the row that is also on screen when the panel is not.
    const float gap = Inventory::OnHand(slot) ? metric.gap : 0.0f;

    return {panel.x + metric.pad + column * (metric.side + metric.pad),
            panel.y + metric.pad + row * (metric.side + metric.pad) + gap, metric.side, metric.side};
}

Rectangle pack::Layout::Tab(int tab) const {
    // Sitting on the panel's top edge and overlapping it by a hair, so the one in front
    // reads as part of the panel rather than as a button floating over it.
    const float tabWide = std::floor(metric.side * 2.4f);
    const float tabTall = std::floor(metric.side * 0.7f);

    return {panel.x + static_cast<float>(tab) * (tabWide + 4.0f), panel.y - tabTall + 2.0f, tabWide, tabTall};
}

Rectangle pack::Layout::StoreSlot(int slot) const {
    const int column = slot % kStoreColumns;
    const int row    = slot / kStoreColumns;

    const float top = store.y + metric.pad + kHeadRows * metric.side + metric.pad;

    return {store.x + metric.pad + column * (metric.side + metric.pad), top + row * (metric.side + metric.pad),
            metric.side, metric.side};
}

Rectangle pack::Layout::Row(int row) const {
    const Rectangle first = StoreSlot(row * kStoreColumns);

    return {store.x + store.width + metric.pad, first.y, metric.side, metric.side};
}
