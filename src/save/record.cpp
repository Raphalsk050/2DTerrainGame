#include "save/record.h"

#include "raylib.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace {

// The one place the layout on disk is spelled out.
constexpr const char *kUnder = "saves";
constexpr const char *kFile  = "world.txt";
constexpr const char *kShot  = "shot.png";

// What a folder name may hold.
//
// Letters, digits, dash and underscore, and nothing else — a save called `../../etc`
// must be a folder called `etc`, and a save called "My World :)" must be a folder a
// filesystem will take. The *name* keeps every character the player typed; only the
// folder is flattened, which is why the two are separate fields on `Slot`.
std::string Flatten(const std::string &name) {
    std::string out;

    for (const char letter : name) {
        const unsigned char c = static_cast<unsigned char>(letter);

        if (std::isalnum(c) != 0) out += static_cast<char>(std::tolower(c));
        else if (!out.empty() && out.back() != '-') out += '-';
    }

    while (!out.empty() && out.back() == '-') out.pop_back();

    if (out.empty()) out = "world";
    if (out.size() > 40) out.resize(40);

    return out;
}

std::string Under(const std::string &id) {
    return std::string(save::Folder()) + "/" + id;
}

} // namespace

const char *save::Folder() {
    return kUnder;
}

std::string save::Slot::Path() const {
    return Under(id) + "/" + kFile;
}

std::string save::Slot::ShotPath() const {
    return Under(id) + "/" + kShot;
}

std::string save::Fresh(const std::string &name) {
    const std::string stem = Flatten(name);

    // The bare name first, then numbered. Two worlds both called "test" are a thing
    // players do constantly, and the second one must not overwrite the first.
    for (int n = 0; n < 1000; n++) {
        const std::string id = (n == 0) ? stem : (stem + "-" + std::to_string(n));

        if (!std::filesystem::exists(Under(id))) return id;
    }

    return stem + "-full";
}

bool save::Peek(const std::string &id, Slot &out) {
    Slot slot;

    slot.id = id;

    Reader in(slot.Path());
    if (!in.Opened()) return false;

    bool head = false;

    // Only as far as the head. A folder of twenty saves listed by reading twenty
    // journals is a menu that takes a second to open, and the head is the whole of
    // what a row on that menu shows.
    while (in.Next()) {
        if (in.Is("save")) {
            const long long version = in.Int();

            if (version != 1 || !in.Ok()) return false;

            head = true;
            continue;
        }

        if (in.Is("name")) slot.name = in.Text();
        if (in.Is("seed")) slot.seed = static_cast<int>(in.Int());
        if (in.Is("mode")) slot.creative = in.Text() == "creative";
        if (in.Is("clock")) slot.clock = in.Real();
        if (in.Is("written")) slot.written = in.Int();

        // The head is every record before the first section, and `player` is the
        // first thing that is not one.
        if (in.Is("player")) break;
    }

    if (!head || !in.Ok()) return false;

    if (slot.name.empty()) slot.name = id;

    slot.shot = FileExists(slot.ShotPath().c_str());

    out = slot;

    return true;
}

std::vector<save::Slot> save::List() {
    std::vector<Slot> found;

    std::error_code oops;

    // The error code rather than the throwing overload, because a saves folder that is
    // not there is the ordinary case on a first run and is not an error — and a menu
    // that throws on the way to being drawn is a game that cannot open.
    for (const auto &entry : std::filesystem::directory_iterator(kUnder, oops)) {
        if (!entry.is_directory()) continue;

        Slot slot;

        if (Peek(entry.path().filename().string(), slot)) found.push_back(slot);
    }

    // Newest first, which is the one a player wants nine times in ten. Ties broken by
    // name so the order is a function of the saves and not of the order the
    // filesystem happened to hand them over — a list that reshuffles itself between
    // two openings is a list nobody can point at.
    std::sort(found.begin(), found.end(), [](const Slot &a, const Slot &b) {
        if (a.written != b.written) return a.written > b.written;

        return a.name < b.name;
    });

    return found;
}

bool save::Rename(const std::string &id, const std::string &name) {
    Slot slot;

    slot.id = id;

    // Read whole, one line swapped, written back. A save is a few megabytes at worst
    // and a rename is a thing a player does once, so the simple way is the right way —
    // and it keeps every other record byte for byte, which is what makes `--saves`
    // able to check that renaming changes nothing but the name.
    std::FILE *file = std::fopen(slot.Path().c_str(), "rb");
    if (file == nullptr) return false;

    std::vector<std::string> lines;

    std::string line;

    for (int c = std::fgetc(file); c != EOF; c = std::fgetc(file)) {
        if (c == '\n') {
            lines.push_back(line);
            line.clear();

            continue;
        }

        if (c != '\r') line += static_cast<char>(c);
    }

    if (!line.empty()) lines.push_back(line);

    std::fclose(file);

    bool found = false;

    for (std::string &at : lines) {
        if (at.rfind("name ", 0) != 0) continue;

        at    = "name \"" + name + "\"";
        found = true;

        break;
    }

    if (!found) return false;

    const std::string interim = slot.Path() + ".part";

    std::FILE *out = std::fopen(interim.c_str(), "wb");
    if (out == nullptr) return false;

    // Written with the same single byte the writer ends its records with, and never
    // the platform's idea of a line ending — a renamed save has to stay byte for byte
    // the file it was, or the check that says so has nothing to compare.
    for (const std::string &at : lines) std::fprintf(out, "%s\n", at.c_str());

    std::fclose(out);

    std::error_code oops;

    std::filesystem::rename(interim, slot.Path(), oops);

    if (oops) {
        std::filesystem::remove(interim, oops);

        return false;
    }

    return true;
}

bool save::Erase(const std::string &id) {
    // Refused unless it looks like one of ours and actually is one. This is the one
    // call in the project that removes a directory, and the whole guard is that `id`
    // came from `List` — but a guard that trusts its caller is not one.
    if (id.empty() || id.find('/') != std::string::npos || id.find('\\') != std::string::npos) return false;
    if (id.find("..") != std::string::npos) return false;

    const std::filesystem::path folder = Under(id);

    // And that it actually is a save: a folder with our world file in it. Between the
    // two tests, the worst a mistyped id can do is fail.
    if (!std::filesystem::is_directory(folder)) return false;
    if (!std::filesystem::exists(folder / kFile)) return false;

    std::error_code oops;

    std::filesystem::remove(folder / kFile, oops);
    std::filesystem::remove(folder / kShot, oops);

    // The folder last, and by `remove` rather than `remove_all`. A save folder that
    // has something else in it is left standing rather than swept out — whatever else
    // is in there is not this program's to delete, and `remove` on a non-empty
    // directory simply fails, which is the behaviour wanted.
    std::filesystem::remove(folder, oops);

    return !std::filesystem::exists(folder / kFile);
}

// ------------------------------------------------------------------ the writer

save::Writer::Writer(const std::string &path) {
    // The folder made on the way in, so a caller never has to think about it. A save
    // is one folder with two files in it, and the folder is part of the file's own
    // path rather than a separate step somebody can forget.
    std::error_code oops;

    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), oops);

    file_ = std::fopen(path.c_str(), "wb");

    if (file_ == nullptr) failed_ = true;
}

save::Writer::~Writer() {
    if (file_ != nullptr) std::fclose(file_);
}

save::Writer &save::Writer::Tag(const char *tag) {
    if (!Ok()) return *this;

    if (std::fprintf(file_, "%s", tag) < 0) failed_ = true;

    return *this;
}

save::Writer &save::Writer::Int(long long value) {
    if (!Ok()) return *this;

    if (std::fprintf(file_, " %lld", value) < 0) failed_ = true;

    return *this;
}

save::Writer &save::Writer::Real(float value) {
    if (!Ok()) return *this;

    // Nine significant digits, which is the shortest decimal that reads back as the
    // same float for every float there is. Anything shorter makes the round trip
    // nearly exact, and nearly exact is a world that drifts a little every time it is
    // loaded and saved.
    if (std::fprintf(file_, " %.9g", static_cast<double>(value)) < 0) failed_ = true;

    return *this;
}

save::Writer &save::Writer::Flag(bool value) {
    return Int(value ? 1 : 0);
}

save::Writer &save::Writer::Text(const char *value) {
    if (!Ok()) return *this;

    if (std::fprintf(file_, " \"%s\"", (value != nullptr) ? value : "") < 0) failed_ = true;

    return *this;
}

void save::Writer::Done() {
    if (!Ok()) return;

    // Written as one byte and never as the platform's idea of a line ending, because
    // the file is opened binary — a save written on Windows and read on a Mac has to
    // be the same file, and `--saves` compares two of them byte for byte.
    if (std::fputc('\n', file_) == EOF) failed_ = true;
}

// ------------------------------------------------------------------ the reader

save::Reader::Reader(const std::string &path) {
    std::FILE *file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) return;

    opened_ = true;

    std::string line;

    for (int c = std::fgetc(file); c != EOF; c = std::fgetc(file)) {
        if (c == '\n') {
            lines_.push_back(line);
            line.clear();

            continue;
        }

        if (c != '\r') line += static_cast<char>(c);
    }

    if (!line.empty()) lines_.push_back(line);

    std::fclose(file);
}

bool save::Reader::Next() {
    while (at_ < lines_.size()) {
        const std::string &line = lines_[at_];

        at_++;
        line_ = static_cast<int>(at_);

        tag_.clear();
        fields_.clear();
        field_ = 0;

        std::size_t i = 0;

        const auto space = [&line](std::size_t at) { return at < line.size() && line[at] == ' '; };

        while (space(i)) i++;

        if (i >= line.size()) continue;

        // A blank line and a comment are both nothing. Comments are not written by
        // this program and are read anyway, because the first thing anybody does with
        // a text save is annotate one while working out what went wrong.
        if (line[i] == '#') continue;

        while (i < line.size() && line[i] != ' ') tag_ += line[i++];

        while (i < line.size()) {
            while (space(i)) i++;

            if (i >= line.size()) break;

            std::string field;

            if (line[i] == '"') {
                i++;

                while (i < line.size() && line[i] != '"') field += line[i++];

                if (i < line.size()) i++;
                else ok_ = false; // A quote that never closes is a line that was cut short.
            } else {
                while (i < line.size() && line[i] != ' ') field += line[i++];
            }

            fields_.push_back(field);
        }

        return true;
    }

    return false;
}

void save::Reader::Again() {
    if (at_ > 0) at_--;
}

bool save::Reader::Is(const char *tag) const {
    return tag != nullptr && tag_ == tag;
}

bool save::Reader::More() {
    if (field_ < fields_.size()) return true;

    ok_ = false;

    return false;
}

long long save::Reader::Int() {
    if (!More()) return 0;

    return std::strtoll(fields_[field_++].c_str(), nullptr, 10);
}

float save::Reader::Real() {
    if (!More()) return 0.0f;

    return std::strtof(fields_[field_++].c_str(), nullptr);
}

bool save::Reader::Flag() {
    return Int() != 0;
}

std::string save::Reader::Text() {
    if (!More()) return {};

    return fields_[field_++];
}

// ------------------------------------------------------------------- a stack

void save::PutStack(Writer &out, const Stack &stack) {
    if (stack.Empty()) {
        out.Text("");
        out.Text("");
        out.Int(0);
        out.Int(0);

        return;
    }

    out.Text((stack.holds == Holds::Material) ? "material" : "item");
    out.Text(stack.Name());
    out.Int(stack.count);
    out.Int(stack.wear);
}

Stack save::GetStack(Reader &in) {
    const std::string which = in.Text();
    const std::string name  = in.Text();

    const long long count = in.Int();
    const long long wear  = in.Int();

    if (which.empty() || name.empty() || count <= 0) return {};

    if (which == "material") {
        const std::optional<Element> found = ElementNamed(name);

        if (found.has_value()) {
            return {.holds = Holds::Material,
                    .what  = static_cast<std::uint8_t>(ElementIndex(*found)),
                    .wear  = static_cast<std::uint16_t>(wear),
                    .count = static_cast<int>(count)};
        }

        // A material this build has never heard of. Refused rather than dropped: a
        // save written by a build that had one more row in its table is not this
        // game's save, and loading it with a hole in the bag would be a silent loss
        // of whatever the player was carrying.
        in.Fail();

        return {};
    }

    if (which == "item") {
        const std::optional<Item> found = item::Find(name.c_str());

        if (found.has_value()) {
            return {.holds = Holds::Item,
                    .what  = static_cast<std::uint8_t>(found->index),
                    .wear  = static_cast<std::uint16_t>(wear),
                    .count = static_cast<int>(count)};
        }
    }

    in.Fail();

    return {};
}
