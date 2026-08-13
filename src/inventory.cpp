#include "inventory.h"

#include "hotbar.h"
#include "picture.h"

#include <algorithm>
#include <cmath>

namespace {

// The panel's own spacing. The slots are the bar's, so that a row of the grid
// and the bar under it are the same row twice and not two rows that happen to
// look alike.
constexpr float kGap = 14.0f;

constexpr Color kPanel = {24, 27, 34, 245};
constexpr Color kEdge  = {96, 104, 120, 255};
constexpr Color kTip   = {18, 20, 26, 235};
constexpr Color kCount = {255, 214, 110, 255};

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
    DrawText(text, static_cast<int>(box.x + 8.0f), static_cast<int>(box.y + 5.0f), 14, RAYWHITE);
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

Inventory::Gesture Inventory::Update() {
    Gesture gesture{};

    const bool left  = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    const bool right = IsMouseButtonPressed(MOUSE_BUTTON_RIGHT);

    if (!left && !right) return gesture;

    const Vector2 mouse = GetMousePosition();

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

void Inventory::Draw() const {
    const Rectangle panel = Bounds();

    DrawRectangleRec(panel, kPanel);
    DrawRectangleLinesEx(panel, 2.0f, kEdge);

    for (int slot = kOnHand; slot < kSlots; slot++) {
        hotbar::DrawSlot(slots_[static_cast<std::size_t>(slot)], SlotBounds(slot), false);
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

            const Stack &stack = slots_[static_cast<std::size_t>(slot)];
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

    DrawText(amount, x + 1, y + 1, 10, {12, 14, 18, 220});
    DrawText(amount, x, y, 10, kCount);
}
