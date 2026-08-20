#pragma once

#include "core/stack.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// How a save is written down, and nothing about what is in one.
//
// The split is the whole design of this module. **The format lives here; the
// contents live with the thing that knows them.** `World::Save` writes the world's
// journal, `Grove::Save` writes the wood, `Fixtures::Save` writes what is standing
// and what is in it — each of them beside the fields it is writing, so that adding a
// field to `TreeState` means editing the file you were already editing. The
// alternative is one enormous file that reaches into six others' privates, and the
// first thing it does is make every one of those fields public.
//
// ---
//
// **It is text, and that is a decision rather than laziness.** Three reasons, in the
// order they mattered:
//
//   - **Names, never indices.** §19.1 is the long version: a row's id is a function
//     of the *set* of rows, so it moves the day a material is added, and a binary
//     save keyed on ids is a world that quietly turns to stone where the diamond was
//     the next time somebody adds a row. Every reference in here is the row's own
//     name, which is the one thing about a row that is already required to be what it
//     says.
//   - **A save that cannot be read is a save that is lost.** Text can be looked at,
//     diffed, and repaired by hand. There is no version of that for a wall of bytes.
//   - **It can be held to §2's bar.** Write, read, write again, compare the two files
//     byte for byte — a round trip that either is exact or names the line where it
//     stopped being. `--saves` is that check, and it only works cleanly because the
//     serialisation is deterministic text.
//
// The cost is size, and it was measured rather than feared: a heavily dug world is a
// few megabytes, which is the same order as one screenshot.
//
// ---
//
// **A record is a line**: a tag, then its fields in order, separated by spaces.
// Anything that could hold a space — every name in the game, and "wood plank" is one
// — is written in quotes. Nothing else needs escaping, because nothing else in this
// game's content is allowed a quote in its name.
namespace save {

// Where saves live, beside the executable — which is where the working directory is
// moved to at startup, so that a game started by double-clicking it writes its worlds
// next to itself rather than into whatever folder the shell happened to be in.
const char *Folder();

// What one save says about itself before it is opened.
//
// The head of the file, and only the head: listing a folder of saves must not mean
// reading a megabyte of journal per row. `List` stops as soon as it has these.
struct Slot {
    // The folder under `Folder()`, which is what every other call names a save by. It
    // is derived from the name once, when the world is made, and never again — so
    // renaming a save is a line in a file rather than a directory that has to be moved
    // while something might be reading it.
    std::string id;

    std::string name;

    int seed      = 0;
    bool creative = false;

    // Seconds on the weather clock, and the wall clock when it was last written.
    float clock = 0.0f;

    std::int64_t written = 0;

    // Whether a preview picture is beside it.
    bool shot = false;

    std::string Path() const;
    std::string ShotPath() const;
};

// Every save on disk, newest first.
//
// Newest first because that is the one a player wants nine times in ten, and because
// a list sorted by name puts "a test" above the world somebody has played for a
// week.
std::vector<Slot> List();

// The head of one save, without reading the rest of it.
bool Peek(const std::string &id, Slot &out);

// A folder name for a new save called `name`, not already taken.
std::string Fresh(const std::string &name);

// Changes what a save is called, leaving everything else in it exactly as it was.
//
// The name is one record at the head of the file, so this rewrites the file with that
// one line swapped — through the same temporary-and-move the whole save is written by,
// because a rename interrupted halfway would destroy a world for the sake of a word.
//
// The folder keeps the name it was made under. That is deliberate: a folder that
// followed the name would have to be moved while something might be reading it, and
// every save the player has ever taken a note of would change its path the first time
// they corrected a typo.
bool Rename(const std::string &id, const std::string &name);

// Takes one away, picture and all. Refuses anything that is not a save folder under
// `Folder()`, which is the one guard between a mistyped id and somebody's documents.
bool Erase(const std::string &id);

// Writes one line of a save.
//
// It owns the file: opened by the constructor and closed by the destructor, so a
// write that throws or returns early cannot leave a half-open handle on a file the
// next save is about to replace.
class Writer {
public:
    explicit Writer(const std::string &path);
    ~Writer();

    Writer(const Writer &)            = delete;
    Writer &operator=(const Writer &) = delete;

    bool Ok() const { return file_ != nullptr && !failed_; }

    // Starts a record. Every field after it belongs to this line until `Done`.
    Writer &Tag(const char *tag);

    Writer &Int(long long value);

    // `%.9g`, which is the shortest form that reads back as the same float — so the
    // round trip in `--saves` is exact rather than nearly exact, and a world reloaded
    // is the world that was saved rather than one a thousandth away from it.
    Writer &Real(float value);

    Writer &Flag(bool value);

    // Quoted, always, even where the text has no space in it. A field that is
    // sometimes quoted is a reader that has to guess.
    Writer &Text(const char *value);
    Writer &Text(const std::string &value) { return Text(value.c_str()); }

    void Done();

private:
    std::FILE *file_ = nullptr;
    bool failed_     = false;
};

// Reads one back.
//
// The whole file is taken into memory first. A save is small enough that this is not
// worth avoiding, and what it buys is that a malformed line can be reported with its
// number without the file having to be walked twice.
class Reader {
public:
    explicit Reader(const std::string &path);

    bool Opened() const { return opened_; }

    // Moves to the next record. False at the end of the file.
    bool Next();

    // Puts the record just read back, so the next `Next` returns it again.
    //
    // One record of pushback is what lets a section read its own lines and stop at
    // somebody else's without the caller having to say in advance how many there are.
    // A count on the header would work and is worse: it is a number that has to agree
    // with the lines under it, and the first hand-edited save would have it disagree.
    void Again();

    bool Is(const char *tag) const;

    const std::string &Tag() const { return tag_; }

    // Which line the reader is standing on, for a complaint that names it.
    int Line() const { return line_; }

    long long Int();
    float Real();
    bool Flag();
    std::string Text();

    // Whether every field asked for so far was there.
    //
    // Checked once at the end of a section rather than at every field, because the
    // answer to a missing field is always the same — refuse the save — and testing it
    // per field would put that answer in fifty places. A reader that has gone wrong
    // returns zeroes, so a caller that forgets to ask gets a defensible world rather
    // than a crash, and `Ok` is what says it is not the saved one.
    bool Ok() const { return ok_; }

    void Fail() { ok_ = false; }

private:
    bool More();

    std::vector<std::string> lines_;

    std::size_t at_ = 0;
    int line_       = 0;

    std::string tag_;
    std::vector<std::string> fields_;
    std::size_t field_ = 0;

    bool opened_ = false;
    bool ok_     = true;
};

// One slot of an inventory or a chest, by name.
//
// Here rather than in `core/stack.h` because it is about the *save* and not about a
// stack: it is the one place that knows a material is written as `material "rock"`
// and an item as `item "torch"`, and there are four callers of it.
void PutStack(Writer &out, const Stack &stack);

// Back again. An empty slot reads as empty; a name that no longer exists in either
// table is a fault and fails the reader, because the alternative is a world that
// loads with a hole in it and nothing saying why.
Stack GetStack(Reader &in);

} // namespace save
