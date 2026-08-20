#include "item/inventory.h"

#include "ui/hotbar.h"
#include "item/icon.h"
#include "core/picture.h"
#include "ui/pack.h"
#include "save/record.h"
#include "ui/skin.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

// The interface's own, from `ui/skin.h`. See the head of that file for why they are
// not written out here.
constexpr Color kPanel = skin::kPanel;
constexpr Color kEdge  = skin::kEdge;
constexpr Color kTip   = skin::kTip;
constexpr Color kCount = skin::kAccent;

// The bin's own colour, and the one place in this interface that red is not a
// requirement being unmet.
//
// It has to be told apart from a slot at a glance and without being read, because
// unlike every other square on the panel a click on it cannot be undone: what goes in
// is gone. A dark red well with a paler lid drawn over it is the whole of the picture,
// which is enough — Minecraft's is a fire and Terraria's a bin, and both are read by
// their colour long before their shape.
constexpr Color kBinWell = {62, 30, 34, 245};
constexpr Color kBinLid  = {188, 96, 88, 255};

} // namespace

void Inventory::DrawTip(const Stack &stack, Vector2 mouse, int font) {
    // The bar answers "how much is left" at a glance and this answers "how much
    // exactly", which is the question a player asks once they have decided the answer
    // matters. Both, rather than either: a bar alone cannot be compared between two
    // pickaxes, and a number alone is not readable while digging.
    const char *text = stack.Wears() ? TextFormat("%s  %d/%d", stack.Name(), stack.Left(), stack.Lasts())
                     : (stack.count > 1) ? TextFormat("%s  x%d", stack.Name(), stack.count)
                                         : stack.Name();

    const float width = static_cast<float>(MeasureText(text, font));

    Rectangle box = {mouse.x + 14.0f, mouse.y + 14.0f, width + 16.0f, static_cast<float>(font) + 10.0f};

    // Held inside the frame. A tip that runs off the edge is unreadable for
    // exactly the slots at the edge, which are as likely to be the ones being
    // asked about as any other.
    box.x = std::floor(std::min(box.x, static_cast<float>(GetScreenWidth()) - box.width - 4.0f));
    box.y = std::floor(std::min(box.y, static_cast<float>(GetScreenHeight()) - box.height - 4.0f));

    DrawRectangleRec(box, kTip);
    DrawRectangleLinesEx(box, 1.0f, kEdge);
    DrawText(text, static_cast<int>(box.x + 8.0f), static_cast<int>(box.y + 5.0f), font, skin::kText);
}

void Inventory::Sweep(int slot, slots::Bank *store) {
    slots::Bank run = Run();

    Stack moving = run.Take(slot, At(slot).count);
    if (moving.Empty()) return;

    // Into the chest first where one is open. That is what a shift-click means in
    // every game that has a chest, and it is also the only reading that does not
    // waste the gesture: the two halves of the pack are both on screen and a stack can
    // be dragged between them, while the store is the container the player opened the
    // panel to fill.
    if (store != nullptr && !store->Empty()) {
        store->Fill(moving, 0, store->Size());
    } else if (OnHand(slot)) {
        run.Fill(moving, kOnHand, kSlots);
    } else {
        run.Fill(moving, 0, kOnHand);
    }

    // Whatever the other side had no room for goes back where it came from,
    // which is empty again by now. A sweep that half worked has to leave the
    // remainder somewhere, and the slot the player pointed at is the one place
    // they will think to look.
    if (moving.count > 0) run.Put(slot, moving);
}

int Inventory::Add(Stack stack) {
    // Over the whole run, which reaches the bar before the grid because the bar
    // is at the front of it. That puts what was just dug where the hand already
    // is, which is the answer for a player who was digging in order to build —
    // and a player who wanted it elsewhere moves it once.
    return Run().Add(stack);
}

int Inventory::Room(const Stack &stack) const {
    return const_cast<Inventory *>(this)->Run().Room(stack);
}

int Inventory::Tally(const Stack &like) const {
    return const_cast<Inventory *>(this)->Run().Tally(like);
}

bool Inventory::Remove(const Stack &what) {
    return Run().Remove(what);
}

Stack Inventory::Take(int slot, int count) {
    return Run().Take(slot, count);
}

Stack Inventory::Put(int slot, Stack stack) {
    return Run().Put(slot, stack);
}

void Inventory::Select(int slot) {
    selected_ = ((slot % kOnHand) + kOnHand) % kOnHand;
}

Stack Inventory::Release() {
    const Stack away = carried_;

    carried_ = {};

    return away;
}

bool Inventory::WearHeld(int by) {
    if (by <= 0) return false;

    Stack &held = slots_[static_cast<std::size_t>(selected_)];

    if (held.Empty() || !held.Wears()) return false;

    // Saturating, because `wear` is sixteen bits and the count that comes in is a
    // number of blows. Nothing in the game can land sixty-five thousand of them in one
    // frame, and a wrap would hand the player a brand new pickaxe for the last swing of
    // an old one.
    const int worn = std::min<int>(held.wear + by, std::numeric_limits<std::uint16_t>::max());

    held.wear = static_cast<std::uint16_t>(worn);

    if (!held.Spent()) return false;

    // Gone, and gone entirely rather than left at a count of zero: an emptied slot that
    // still remembered what used to be in it would merge with the next one of those to
    // come past and refuse every other — Take's own reasoning, one line further on.
    held = {};

    return true;
}

void Inventory::Save(save::Writer &out) const {
    out.Tag("pack").Int(selected_).Done();

    // Only what is in them, and each with its index. A run of thirty-six lines mostly
    // saying "nothing" is a file that is nine tenths padding, and an index means the
    // reader does not have to trust the count.
    for (int slot = 0; slot < kSlots; slot++) {
        if (slots_[static_cast<std::size_t>(slot)].Empty()) continue;

        out.Tag("slot").Int(slot);

        save::PutStack(out, slots_[static_cast<std::size_t>(slot)]);

        out.Done();
    }
}

void Inventory::Load(save::Reader &in) {
    selected_ = static_cast<int>(in.Int());

    // Clamped rather than trusted, because this one is read straight into an index the
    // hand acts through: a selected slot past the end of the bar is every swing, every
    // placement and every wear check reading off the end of the array.
    Select(selected_);

    slots_.fill({});

    while (in.Next()) {
        if (!in.Is("slot")) {
            in.Again();
            break;
        }

        const long long slot = in.Int();
        const Stack stack    = save::GetStack(in);

        if (!in.Ok()) return;

        if (slot >= 0 && slot < kSlots) slots_[static_cast<std::size_t>(slot)] = stack;
    }
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

const char *Inventory::NameOf(Tab tab) {
    switch (tab) {
    case Tab::Blocks: return "blocks";
    case Tab::Nature: return "nature";
    case Tab::Gear: return "gear";
    case Tab::Count: break;
    }

    return "";
}

Inventory::Page Inventory::PageOf(Tab tab) {
    Page page;

    const auto add = [&page](Stack stack) {
        page.wanted++;

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

Inventory::Gesture Inventory::Update(Gamemode mode, const pack::Layout &at, slots::Bank *store) {
    Gesture gesture{};

    const bool left  = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    const bool right = IsMouseButtonPressed(MOUSE_BUTTON_RIGHT);

    if (!left && !right) return gesture;

    const Vector2 mouse = GetMousePosition();

    // The tabs and the bin stand outside the panel's own rectangle, so both are asked
    // about before the test that decides a click was aimed at the world.
    if (mode == Gamemode::Creative && left) {
        for (int tab = 0; tab < kTabs; tab++) {
            if (!CheckCollisionPointRec(mouse, at.Tab(tab))) continue;

            tab_ = static_cast<Tab>(tab);

            gesture.took = true;
            return gesture;
        }
    }

    // The bin. Only ever with a full hand: it is the one square on the panel a click
    // cannot be taken back from, so it does nothing at all to an empty one rather than
    // opening some second gesture over it.
    //
    // Destroying rather than reporting, unlike a stack thrown at the sky. A thrown
    // stack lands on the ground and can be picked up again, which is the whole
    // difference between the two and the reason both exist.
    if (CheckCollisionPointRec(mouse, at.trash)) {
        gesture.took = true;

        if (left && !carried_.Empty()) carried_ = {};

        // One at a time off the right button, which is the gesture the panel already
        // uses for putting one down and is what makes a bin usable for trimming a
        // stack rather than only for losing one.
        if (right && !carried_.Empty()) {
            carried_.count--;

            if (carried_.count <= 0) carried_ = {};
        }

        return gesture;
    }

    if (!CheckCollisionPointRec(mouse, at.panel)) {
        // Outside the panel — and outside the chest beside it, which the caller has
        // already had its turn at. With a full hand that is a throw and the panel
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

    gesture.took = true;

    int over = -1;

    for (int slot = 0; slot < kSlots; slot++) {
        if (CheckCollisionPointRec(mouse, at.Slot(slot))) {
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

        if (sweeping && carried_.Empty()) Sweep(over, store);
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

    Put(over, carried_.Some(1));

    carried_.count--;
    if (carried_.count <= 0) carried_ = {};

    return gesture;
}

void Inventory::Draw(Gamemode mode, const pack::Layout &at) const {
    const Rectangle panel = at.panel;

    const bool creative = mode == Gamemode::Creative;

    DrawRectangleRec(panel, kPanel);
    DrawRectangleLinesEx(panel, 2.0f, kEdge);

    if (creative) {
        // The tabs first, so the panel's own border is drawn over their bottom edge
        // and the page reads as hanging from the one in front.
        for (int tab = 0; tab < kTabs; tab++) {
            const Rectangle box  = at.Tab(tab);
            const bool showing   = static_cast<Tab>(tab) == tab_;
            const Color face     = showing ? kPanel : Color{18, 20, 26, 245};

            DrawRectangleRec(box, face);
            DrawRectangleLinesEx(box, 2.0f, kEdge);

            const char *name = Inventory::NameOf(static_cast<Tab>(tab));
            const int wide   = MeasureText(name, at.metric.font);

            DrawText(name, static_cast<int>(box.x + (box.width - static_cast<float>(wide)) / 2.0f),
                     static_cast<int>(box.y + (box.height - at.metric.font) / 2.0f), at.metric.font,
                     showing ? skin::kText : skin::kMuted);
        }
    }

    // The bin. Drawn as a well with a lid over it rather than as a slot with a picture
    // in it, because it is the one square here that never holds anything — a slot that
    // is always empty reads as a slot the player has failed to fill.
    {
        const Rectangle bin = at.trash;
        const float lid     = std::floor(bin.height * 0.22f);

        DrawRectangleRec(bin, kBinWell);
        DrawRectangleRec({bin.x + 2.0f, bin.y + lid, bin.width - 4.0f, lid}, kBinLid);
        DrawRectangleLinesEx(bin, 1.0f, kEdge);
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

        hotbar::DrawSlot(stack, at.Slot(slot), false, at.metric.pixel);
    }

    // The bar keeps its ring while the panel is open, so that putting something
    // into the slot that is in hand is a thing that can be aimed at.
    for (int slot = 0; slot < kOnHand; slot++) {
        hotbar::DrawSlot(slots_[static_cast<std::size_t>(slot)], at.Slot(slot), slot == selected_, at.metric.pixel);
    }

    const Vector2 mouse = GetMousePosition();

    // What the bin is, in words, whenever the pointer is over it.
    //
    // Said even with a full hand, unlike everything else here — which is the one place
    // this panel breaks its own rule and does it deliberately. A tip about the slot
    // under a carried stack answers a question nobody asked; a tip about the bin answers
    // the only question that matters at that moment, which is whether letting go here
    // destroys what is being held. A red square is not self-evident and nothing about
    // this one can be taken back.
    if (CheckCollisionPointRec(mouse, at.trash)) {
        const char *says = carried_.Empty() ? "bin — drop a stack here to destroy it" : "destroy it";
        const float wide = static_cast<float>(MeasureText(says, at.metric.font));

        Rectangle box = {mouse.x + 14.0f, mouse.y + 14.0f, wide + 16.0f, static_cast<float>(at.metric.font) + 10.0f};

        box.x = std::floor(std::min(box.x, static_cast<float>(GetScreenWidth()) - box.width - 4.0f));
        box.y = std::floor(std::min(box.y, static_cast<float>(GetScreenHeight()) - box.height - 4.0f));

        DrawRectangleRec(box, kTip);
        DrawRectangleLinesEx(box, 1.0f, kEdge);
        DrawText(says, static_cast<int>(box.x + 8.0f), static_cast<int>(box.y + 5.0f), at.metric.font, kBinLid);

        return;
    }

    // The name of what is under the cursor, but not while something is on the
    // cursor: the hand is already over the slot it is about to go into, and a
    // tip about the slot underneath is an answer to a question nobody asked.
    if (!carried_.Empty()) return;

    for (int slot = 0; slot < kSlots; slot++) {
        if (!CheckCollisionPointRec(mouse, at.Slot(slot))) continue;

        const int index = slot - kOnHand;

        const Stack &stack = (creative && !OnHand(slot))
                               ? ((index < page.count) ? page.at[static_cast<std::size_t>(index)] : Stack{})
                               : slots_[static_cast<std::size_t>(slot)];

        if (!stack.Empty()) DrawTip(stack, mouse, at.metric.font);

        break;
    }
}

void Inventory::DrawCarried(const pack::Layout &at) const {
    if (carried_.Empty()) return;

    const Vector2 mouse = GetMousePosition();

    // Last of all, and centred on the pointer rather than offset from it. The
    // stack is what the pointer *is* while it is held, so anything the cursor is
    // over it has to be drawn over as well.
    const float pixel = at.metric.pixel;
    const float drawn = pixel * kPictureSide;

    const Vector2 corner = {std::floor(mouse.x - drawn / 2.0f), std::floor(mouse.y - drawn / 2.0f)};

    icon::Draw(carried_, corner, pixel);
    icon::DrawWear(carried_, corner, pixel);

    if (carried_.count <= 1) return;

    const char *amount = TextFormat("%d", carried_.count);

    const int x = static_cast<int>(corner.x + drawn - MeasureText(amount, 10) - 1.0f);
    const int y = static_cast<int>(corner.y + drawn - 9.0f);

    DrawText(amount, x + 1, y + 1, 10, skin::kShadow);
    DrawText(amount, x, y, 10, kCount);
}
