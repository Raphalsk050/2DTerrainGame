#include "item/inventory.h"

#include "ui/hotbar.h"
#include "core/picture.h"
#include "ui/skin.h"

#include <algorithm>
#include <cmath>

namespace {

// The panel's own spacing. The slots are the bar's, so that a row of the grid
// and the bar under it are the same row twice and not two rows that happen to
// look alike.
constexpr float kGap = 14.0f;

// The interface's own, from `ui/skin.h`. See the head of that file for why they are
// not written out here.
constexpr Color kPanel = skin::kPanel;
constexpr Color kEdge  = skin::kEdge;
constexpr Color kTip   = skin::kTip;
constexpr Color kCount = skin::kAccent;

// The tabs' own metrics: how tall the strip over the panel is and how wide one
// tab is. Written here rather than shared with the slots, because a tab is a
// label and a slot is a picture and nothing is gained by making them the same
// size.
constexpr float kTabHigh = 30.0f;
constexpr float kTabWide = 104.0f;

const char *NameOf(Inventory::Tab tab) {
    switch (tab) {
    case Inventory::Tab::Blocks: return "blocks";
    case Inventory::Tab::Nature: return "nature";
    case Inventory::Tab::Gear: return "gear";
    case Inventory::Tab::Count: break;
    }

    return "";
}

// The name of what the cursor is over, beside the cursor.
void DrawTip(const Stack &stack, Vector2 mouse) {
    const char *text = (stack.count > 1) ? TextFormat("%s  x%d", stack.Name(), stack.count) : stack.Name();

    const float width = static_cast<float>(MeasureText(text, 14));

    Rectangle box = {mouse.x + 14.0f, mouse.y + 14.0f, width + 16.0f, 24.0f};

    // Held inside the frame. A tip that runs off the edge is unreadable for
    // exactly the slots at the edge, which are as likely to be the ones being
    // asked about as any other.
    box.x = std::floor(std::min(box.x, static_cast<float>(GetScreenWidth()) - box.width - 4.0f));
    box.y = std::floor(std::min(box.y, static_cast<float>(GetScreenHeight()) - box.height - 4.0f));

    DrawRectangleRec(box, kTip);
    DrawRectangleLinesEx(box, 1.0f, kEdge);
    DrawText(text, static_cast<int>(box.x + 8.0f), static_cast<int>(box.y + 5.0f), 14, skin::kText);
}

} // namespace

void Inventory::Fill(Stack &stack, int from, int upto) {
    if (stack.Empty()) return;

    // Onto a stack of the same thing before an empty slot.
    //
    // Topping up first is the half that matters: filling empty slots first would
    // scatter one material over four of them while a half-full one of it sat in
    // the bar.
    for (int slot = from; slot < upto && stack.count > 0; slot++) {
        Stack &into = slots_[static_cast<std::size_t>(slot)];
        if (!into.Alike(stack)) continue;

        const int fits = std::min(into.Room(), stack.count);

        into.count += fits;
        stack.count -= fits;
    }

    for (int slot = from; slot < upto && stack.count > 0; slot++) {
        Stack &into = slots_[static_cast<std::size_t>(slot)];
        if (!into.Empty()) continue;

        const int fits = std::min(stack.Limit(), stack.count);

        into = {.holds = stack.holds, .what = stack.what, .count = fits};
        stack.count -= fits;
    }
}

int Inventory::Add(Stack stack) {
    // Over the whole run, which reaches the bar before the grid because the bar
    // is at the front of it. That puts what was just dug where the hand already
    // is, which is the answer for a player who was digging in order to build —
    // and a player who wanted it elsewhere moves it once.
    Fill(stack, 0, kSlots);

    return stack.count;
}

void Inventory::Sweep(int slot) {
    Stack moving = Take(slot, At(slot).count);
    if (moving.Empty()) return;

    if (OnHand(slot)) Fill(moving, kOnHand, kSlots);
    else Fill(moving, 0, kOnHand);

    // Whatever the other half had no room for goes back where it came from,
    // which is empty again by now. A sweep that half worked has to leave the
    // remainder somewhere, and the slot the player pointed at is the one place
    // they will think to look.
    if (moving.count > 0) Put(slot, moving);
}

int Inventory::Room(const Stack &stack) const {
    if (stack.Empty()) return 0;

    int room = 0;

    for (const Stack &slot : slots_) {
        if (slot.Alike(stack)) room += slot.Room();
        else if (slot.Empty()) room += stack.Limit();
    }

    return room;
}

int Inventory::Tally(const Stack &like) const {
    int held = 0;

    for (const Stack &slot : slots_) {
        if (slot.Alike(like)) held += slot.count;
    }

    return held;
}

bool Inventory::Remove(const Stack &what) {
    if (what.Empty() || Tally(what) < what.count) return false;

    int owing = what.count;

    // From the back, so what is spent comes out of the store before it comes out
    // of the bar. The bar is what the player arranged; the grid is where the
    // spare went.
    for (int slot = kSlots - 1; slot >= 0 && owing > 0; slot--) {
        Stack &from = slots_[static_cast<std::size_t>(slot)];
        if (!from.Alike(what)) continue;

        const int taken = std::min(from.count, owing);

        from.count -= taken;
        owing -= taken;

        if (from.count <= 0) from = {};
    }

    return true;
}

Stack Inventory::Take(int slot, int count) {
    Stack &from = slots_[static_cast<std::size_t>(slot)];

    if (from.Empty() || count <= 0) return {};

    const int taken = std::min(count, from.count);
    const Stack away = {.holds = from.holds, .what = from.what, .count = taken};

    from.count -= taken;

    // Emptied all the way back to nothing rather than left holding a row with a
    // count of zero. Alike() and Empty() both read the count, but an empty slot
    // that still remembers what used to be in it would merge with the next stack
    // of that thing to come past and refuse every other, which is a slot that is
    // full of nothing in particular.
    if (from.count <= 0) from = {};

    return away;
}

Stack Inventory::Put(int slot, Stack stack) {
    Stack &into = slots_[static_cast<std::size_t>(slot)];

    if (stack.Empty()) return {};

    if (into.Empty()) {
        const int fits = std::min(stack.Limit(), stack.count);

        into = {.holds = stack.holds, .what = stack.what, .count = fits};
        stack.count -= fits;

        return (stack.count > 0) ? stack : Stack{};
    }

    if (into.Alike(stack)) {
        const int fits = std::min(into.Room(), stack.count);

        into.count += fits;
        stack.count -= fits;

        // What is left over stays on the cursor rather than going anywhere else.
        // A stack that overflowed into some other slot the player was not
        // looking at is a stack that has moved on its own.
        return (stack.count > 0) ? stack : Stack{};
    }

    // Two different things, so they exchange places. This is what makes a slot
    // reachable in one gesture when it is already occupied.
    const Stack displaced = into;
    into                  = stack;

    return displaced;
}

void Inventory::Select(int slot) {
    selected_ = ((slot % kOnHand) + kOnHand) % kOnHand;
}

Stack Inventory::Release() {
    const Stack away = carried_;

    carried_ = {};

    return away;
}

void Inventory::Clear() {
    slots_.fill({});
    carried_ = {};
}

void Inventory::Stock() {
    for (std::size_t e = 0; e < kElementCount; e++) {
        Add(BlocksOf(static_cast<Element>(e), kElements[e].stack));
    }

    // And a handful of every item, which is mostly about the saplings: there are
    // one per tree now, and trying them out otherwise means finding and felling
    // four different woods first.
    //
    // A handful and not a full stack, unlike the materials above. A material is
    // spent by the fistful under a brush and a stack of it is one wall; an item
    // goes into the world one at a time, and a dozen is more than enough to answer
    // any question about one.
    constexpr int kFew = 12;

    for (int i = 0; i < item::Count(); i++) Add(ItemsOf(Item{i}, kFew));
}

Rectangle Inventory::Bounds() {
    const float side = hotbar::kSlotSide;
    const float pad  = hotbar::kPadding;

    const float width = kColumns * side + (kColumns + 1) * pad;

    // Three rows, the gap, and the bar row under it.
    //
    // Each grid row costs a slot and the padding after it, which is what
    // SlotBounds steps by — counting the padding between rows instead leaves the
    // total one short, and the bar row comes out flush with the bottom edge with
    // its counts running into the border.
    const float height = pad + kRows * (side + pad) + kGap + side + pad;

    // Centred on the frame rather than sat above the bar, and the bar is not
    // drawn behind it while it is open — the panel carries its own copy of those
    // nine slots, and two of them on screen at once is two places to click for
    // one thing.
    return {std::floor((GetScreenWidth() - width) / 2.0f), std::floor((GetScreenHeight() - height) / 2.0f), width,
            height};
}

Rectangle Inventory::SlotBounds(int slot) {
    const Rectangle panel = Bounds();

    const float side = hotbar::kSlotSide;
    const float pad  = hotbar::kPadding;

    const int column = OnHand(slot) ? slot : (slot - kOnHand) % kColumns;
    const int row    = OnHand(slot) ? kRows : (slot - kOnHand) / kColumns;

    // The bar row is one row further down plus the gap, which is what sets it
    // apart as the row that is also on screen when the panel is not.
    const float gap = OnHand(slot) ? kGap : 0.0f;

    return {panel.x + pad + column * (side + pad), panel.y + pad + row * (side + pad) + gap, side, side};
}

bool Inventory::Contains(Vector2 screen) {
    return CheckCollisionPointRec(screen, Bounds());
}

Rectangle Inventory::TabBounds(int tab) {
    const Rectangle panel = Bounds();

    // Sitting on the panel's top edge and overlapping it by a hair, so the one in
    // front reads as part of the panel rather than as a button floating over it.
    return {panel.x + static_cast<float>(tab) * (kTabWide + 4.0f), panel.y - kTabHigh + 2.0f, kTabWide, kTabHigh};
}

Inventory::Page Inventory::PageOf(Tab tab) {
    Page page;

    const auto add = [&page](Stack stack) {
        if (page.count >= static_cast<int>(page.at.size())) return;

        page.at[static_cast<std::size_t>(page.count)] = stack;
        page.count++;
    };

    if (tab == Tab::Blocks) {
        // Every material there is, in the table's own order — the same run Stock
        // hands out and for the same reason. Filtering it to the ones that occupy a
        // cell reads sensibly and quietly drops the water, which a hand can already
        // pour in survival: a palette that offers less than the debug key does is a
        // palette with a hole in it.
        for (std::size_t e = 0; e < kElementCount; e++) {
            add(BlocksOf(static_cast<Element>(e), kElements[e].stack));
        }

        return page;
    }

    for (int i = 0; i < item::Count(); i++) {
        const auto item      = static_cast<Item>(i);
        // Gear is what is *used* rather than what is gathered: anything that fixes
        // to a surface, and anything that is a tool. Still derived and not a field
        // on the row — eight tools arrived in one commit and every one of them
        // landed on the right page without being told about this file.
        const bool gear      = Def(item).placement == Placement::Fixture || Def(item).tool.Any();
        const Tab belongs    = gear ? Tab::Gear : Tab::Nature;

        if (belongs != tab) continue;

        add(ItemsOf(item, Def(item).stack));
    }

    return page;
}

Inventory::Gesture Inventory::Update(Gamemode mode) {
    Gesture gesture{};

    const bool left  = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    const bool right = IsMouseButtonPressed(MOUSE_BUTTON_RIGHT);

    if (!left && !right) return gesture;

    const Vector2 mouse = GetMousePosition();

    // The tabs stand outside the panel's own rectangle, so they are asked about
    // before the test that decides a click was aimed at the world.
    if (mode == Gamemode::Creative && left) {
        for (int tab = 0; tab < kTabs; tab++) {
            if (!CheckCollisionPointRec(mouse, TabBounds(tab))) continue;

            tab_ = static_cast<Tab>(tab);
            return gesture;
        }
    }

    if (!Contains(mouse)) {
        // Outside the panel. With a full hand that is a throw and the panel
        // stays open; with an empty one it is a dismissal.
        //
        // Both were asked for on the same gesture, and this is the only reading
        // under which both can happen: a player who is holding something and
        // clicks the sky meant to get rid of it, and a player holding nothing
        // who clicks the sky meant to get out. Which is also what Minecraft
        // does, where the panel closes on a key and never on the throw.
        if (carried_.Empty()) {
            gesture.close = true;
            return gesture;
        }

        gesture.thrown = carried_;
        carried_       = {};

        return gesture;
    }

    int over = -1;

    for (int slot = 0; slot < kSlots; slot++) {
        if (CheckCollisionPointRec(mouse, SlotBounds(slot))) {
            over = slot;
            break;
        }
    }

    // On the panel but between slots, which changes nothing and must not close
    // it either.
    if (over < 0) return gesture;

    // The palette, which is a shelf and not a bag: a click takes a copy of what is
    // on it, and a hand already holding something puts that thing back on the shelf
    // — which is to say it is gone, because the shelf has one of everything
    // already. Minecraft's creative panel does exactly this, and it is what makes
    // the palette a place to throw away as well as a place to take from.
    if (mode == Gamemode::Creative && !OnHand(over)) {
        if (!carried_.Empty()) {
            carried_ = {};
            return gesture;
        }

        const Page page = PageOf(tab_);
        const int index = over - kOnHand;

        if (index < page.count) carried_ = page.at[static_cast<std::size_t>(index)];

        return gesture;
    }

    Stack &into = slots_[static_cast<std::size_t>(over)];

    if (left) {
        const bool sweeping = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);

        if (sweeping && carried_.Empty()) Sweep(over);
        else carried_ = carried_.Empty() ? Take(over, into.count) : Put(over, carried_);

        return gesture;
    }

    // The right button halves and singles: half of a slot comes up onto an empty
    // hand, and one at a time goes back down off a full one. Between them they
    // are how a stack is divided without any arithmetic being asked of the
    // player.
    if (carried_.Empty()) {
        carried_ = Take(over, (into.count + 1) / 2);
        return gesture;
    }

    // Only where the one has somewhere to go. Left to Put, a right click on a
    // slot holding something else would exchange the whole slot for a single
    // item, which is the left button's gesture and not this one.
    if (!into.Empty() && !(into.Alike(carried_) && into.Room() > 0)) return gesture;

    Put(over, {.holds = carried_.holds, .what = carried_.what, .count = 1});

    carried_.count--;
    if (carried_.count <= 0) carried_ = {};

    return gesture;
}

void Inventory::Draw(Gamemode mode) const {
    const Rectangle panel = Bounds();

    const bool creative = mode == Gamemode::Creative;

    DrawRectangleRec(panel, kPanel);
    DrawRectangleLinesEx(panel, 2.0f, kEdge);

    if (creative) {
        // The tabs first, so the panel's own border is drawn over their bottom edge
        // and the page reads as hanging from the one in front.
        for (int tab = 0; tab < kTabs; tab++) {
            const Rectangle at   = TabBounds(tab);
            const bool showing   = static_cast<Tab>(tab) == tab_;
            const Color face     = showing ? kPanel : Color{18, 20, 26, 245};

            DrawRectangleRec(at, face);
            DrawRectangleLinesEx(at, 2.0f, kEdge);

            const char *name = NameOf(static_cast<Tab>(tab));
            const int wide   = MeasureText(name, 14);

            DrawText(name, static_cast<int>(at.x + (at.width - static_cast<float>(wide)) / 2.0f),
                     static_cast<int>(at.y + 8.0f), 14, showing ? skin::kText : skin::kMuted);
        }
    }

    // The grid: the player's own slots in survival, and the page of the palette in
    // creative. One loop, because they are the same twenty-seven squares in the
    // same places and only what stands in them differs.
    const Page page = creative ? PageOf(tab_) : Page{};

    for (int slot = kOnHand; slot < kSlots; slot++) {
        const int index = slot - kOnHand;

        const Stack &stack = creative
                               ? ((index < page.count) ? page.at[static_cast<std::size_t>(index)] : Stack{})
                               : slots_[static_cast<std::size_t>(slot)];

        hotbar::DrawSlot(stack, SlotBounds(slot), false);
    }

    // The bar keeps its ring while the panel is open, so that putting something
    // into the slot that is in hand is a thing that can be aimed at.
    for (int slot = 0; slot < kOnHand; slot++) {
        hotbar::DrawSlot(slots_[static_cast<std::size_t>(slot)], SlotBounds(slot), slot == selected_);
    }

    const Vector2 mouse = GetMousePosition();

    // The name of what is under the cursor, but not while something is on the
    // cursor: the hand is already over the slot it is about to go into, and a
    // tip about the slot underneath is an answer to a question nobody asked.
    if (carried_.Empty()) {
        for (int slot = 0; slot < kSlots; slot++) {
            if (!CheckCollisionPointRec(mouse, SlotBounds(slot))) continue;

            const int index = slot - kOnHand;

            const Stack &stack = (creative && !OnHand(slot))
                                   ? ((index < page.count) ? page.at[static_cast<std::size_t>(index)] : Stack{})
                                   : slots_[static_cast<std::size_t>(slot)];

            if (!stack.Empty()) DrawTip(stack, mouse);

            break;
        }
    }

    if (carried_.Empty()) return;

    // Last of all, and centred on the pointer rather than offset from it. The
    // stack is what the pointer *is* while it is held, so anything the cursor is
    // over it has to be drawn over as well.
    const float pixel = hotbar::kIconPixel;
    const float drawn = pixel * kPictureSide;

    const Vector2 corner = {std::floor(mouse.x - drawn / 2.0f), std::floor(mouse.y - drawn / 2.0f)};

    DrawPicture(PictureOf(carried_), corner, pixel);

    if (carried_.count <= 1) return;

    const char *amount = TextFormat("%d", carried_.count);

    const int x = static_cast<int>(corner.x + drawn - MeasureText(amount, 10) - 1.0f);
    const int y = static_cast<int>(corner.y + drawn - 9.0f);

    DrawText(amount, x + 1, y + 1, 10, skin::kShadow);
    DrawText(amount, x, y, 10, kCount);
}
