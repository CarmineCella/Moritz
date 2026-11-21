// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// timeline_tests.cpp — the score document (undo exact and bounded, listeners, bound to a score box or a .score file,
// saved and read back, one document per score, a placed score opened), the timeline's operations (tracks, move,
// trim, split, placements, lanes, clipboard), the playback of tracks (mute, solo, gain, pan, fades) and of automation
// lanes (their messages on their sample, to the receives), editing while the audio thread plays, and the render; the
// heights of tracks and lanes; the transport of a score's window (ScorePlayback: Play always plays).

#include "rig.h"
#include "score_doc.h"
#include "score_window.h"
#include "timeline_ops.h"
#include "transport.h"

#include "music/score_text.h"

#include <atomic>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <random>
#include <thread>

using namespace moritz;
using etest::Rig;
namespace tl = moritz::timeline;
namespace fs = std::filesystem;

namespace {

void impulse(Rig& r, const char* id, long frames = 4410) {
    auto* b = dynamic_cast<BufferBox*>(r.box(id));
    SampleBuffer s;
    s.channels = 1;
    s.sr = r.sr;
    s.frames = frames;
    s.data.assign((size_t)frames, 0.f);
    s.data[0] = 1.f;
    b->replace(std::move(s));
}
void constant(Rig& r, const char* id, long frames) {
    auto* b = dynamic_cast<BufferBox*>(r.box(id));
    SampleBuffer s;
    s.channels = 1;
    s.sr = r.sr;
    s.frames = frames;
    s.data.assign((size_t)frames, 1.f);
    b->replace(std::move(s));
}
std::vector<long> above(const std::vector<float>& y, float level) {
    std::vector<long> at;
    for (size_t i = 0; i < y.size(); ++i)
        if (y[i] > level) at.push_back((long)i);
    return at;
}
fs::path scratch(const char* name) {
    fs::path dir = fs::temp_directory_path() / (std::string("moritz_tl_") + name + "_" + std::to_string(std::rand()));
    fs::create_directories(dir);
    return dir;
}
// the score as text, but for the next id (never given back: ids are not reused, an undo does not lower it)
std::string text(const sc::Score& s) {
    std::string t = sc::toText(s);
    size_t k = t.find("(next-id ");
    if (k != std::string::npos) t.erase(k, t.find(')', k) - k + 1);
    return t;
}
std::string text(const ScoreDoc& d) { return text(d.score()); }

// a score box (stereo) into the dac, two buffers, a receive into a click on a third channel
const char* kPatch =
    "(patch (obj b buffer~ imp 100) (obj k buffer~ one 100) (obj s score x 2) (obj o dac~ 1 2 3)"
    " (obj tr transport 240 4 4) (obj r r density) (obj tb t b) (obj c click~) (obj p print lane)"
    " (connect s 0 o 0) (connect s 1 o 1) (connect r 0 tb 0) (connect tb 0 c 0) (connect c 0 o 2) (connect r 0 p 0))";

}  // namespace

// Every edit taken back by undo gives the score as it was, text for text; redo gives it again, the same ids
TEST(scoredoc_undo_redo_is_exact) {
    auto d = ScoreDoc::make("x");
    std::mt19937 rng(7);
    std::vector<std::string> states{text(*d)};
    for (int k = 0; k < 300; ++k) {
        const sc::Score& s = d->score();
        int what = (int)(rng() % 6);
        auto e = d->edit("op");
        if (what <= 1 || s.empty()) {
            auto ev = std::make_shared<sc::Event>();
            ev->kind = (sc::Kind)(rng() % 3 == 0 ? 1 : rng() % 2 ? 0 : 4);
            ev->at = (rng() % 1000) / 100.0;
            ev->dur = ev->kind == sc::Kind::Message ? 0 : (rng() % 100) / 50.0;
            ev->row = "r" + std::to_string(rng() % 3);
            ev->source = ev->kind == sc::Kind::Message ? "density" : "a.wav";
            if (ev->kind == sc::Kind::Message) ev->values = {sc::Datum::number((rng() % 100) / 100.0)};
            e.insert(ev);
        } else if (what == 2) {
            e.remove(s.events()[rng() % s.size()]->id);
        } else if (what == 3) {
            sc::Event now = *s.events()[rng() % s.size()];
            now.at += 0.25;
            now.gain = 0.5;
            sc::setField(now.extra, "fade-in", sc::Datum::number(0.1));
            e.replace(now);
        } else if (what == 4) {
            auto rows = s.rows;
            rows.push_back(sc::Row{"t" + std::to_string(k), "", {}});
            e.setRows(rows);
        } else {
            e.setHeader("x", 60 + k % 50, 3 + k % 2, 4);
        }
        CHECK(e.commit());
        states.push_back(text(*d));
    }
    CHECK(d->historySteps() == 300);
    for (int k = 300; k > 0; --k) {
        CHECK(d->undo());
        CHECK(text(*d) == states[(size_t)k - 1]);
    }
    CHECK(!d->undo());
    for (int k = 1; k <= 300; ++k) {
        CHECK(d->redo());
        CHECK(text(*d) == states[(size_t)k]);
    }
    // an edit that fails is taken back whole (a score that would contain itself)
    std::string before = text(*d);
    auto self = std::make_shared<sc::Event>();
    self->kind = sc::Kind::Score;
    self->sub = d->scorePtr();
    auto e = d->edit("bad");
    e.insert(std::make_shared<sc::Event>());
    e.insert(self);
    CHECK(!e.commit());
    CHECK(text(*d) == before);
    CHECK(d->message.find("contain itself") != std::string::npos);
}

// The history keeps at most maxSteps steps and about undoBudget bytes (the last step always)
TEST(scoredoc_history_is_bounded) {
    size_t budget = ScoreDoc::undoBudget, steps = ScoreDoc::maxSteps;
    ScoreDoc::undoBudget = 64 << 10;
    ScoreDoc::maxSteps = 40;
    auto d = ScoreDoc::make("x");
    for (int k = 0; k < 400; ++k) {
        auto ev = std::make_shared<sc::Event>();
        ev->kind = sc::Kind::Sound;
        ev->source = std::string(200, 'a');
        ev->at = k;
        ev->dur = 1;
        auto e = d->edit("Add");
        e.insert(ev);
        e.commit();
    }
    CHECK(d->historySteps() <= 40);
    CHECK(d->historyBytes() <= ScoreDoc::undoBudget);
    CHECK(d->score().size() == 400);
    CHECK(d->dirty());
    while (d->undo()) {}
    CHECK(d->score().size() >= 360);
    CHECK(d->dirty());                             // the oldest steps are gone: never back to the start
    // one huge step is kept whatever its size
    ScoreDoc::undoBudget = 16;
    auto e = d->edit("Big");
    for (int k = 0; k < 50; ++k) {
        auto ev = std::make_shared<sc::Event>();
        ev->at = k;
        e.insert(ev);
    }
    e.commit();
    CHECK(d->canUndo());
    ScoreDoc::undoBudget = budget;
    ScoreDoc::maxSteps = steps;
}

// Listeners hear every change once, with what changed; one may stop listening while being told
TEST(scoredoc_listeners_and_selection) {
    auto d = ScoreDoc::make("x");
    int content = 0, selection = 0, other = 0;
    int t1 = d->listen([&](unsigned w) {
        if (w & ScoreDoc::Content) ++content;
        if (w & ScoreDoc::Selection) ++selection;
    });
    int t2 = 0;
    t2 = d->listen([&](unsigned) { ++other; d->unlisten(t2); });
    auto ev = std::make_shared<sc::Event>();
    ev->dur = 1;
    auto e = d->edit("Add");
    uint64_t id = e.insert(ev);
    e.commit();
    CHECK(content == 1 && other == 1);
    d->select({id, 999});
    CHECK(d->selection().size() == 1 && d->isSelected(id) && selection == 1);
    d->undo();                                      // the event goes: so does its selection
    CHECK(content == 2 && selection == 2 && d->selection().empty());
    d->unlisten(t1);
    d->redo();
    CHECK(content == 2 && other == 1);
}

// Bound to a score box: an edit plays at once (the box's own score, edited in place), undo takes it back; one
// document per box; a message to the box, or the box replaced, is followed; the box gone, the score is kept
TEST(scoredoc_bound_to_a_score_box) {
    Rig r(kPatch);
    impulse(r, "b");
    int edited = 0;
    ScoreDoc::patchEdited = [&](Patch*) { ++edited; };
    auto d = ScoreDoc::ofBox(r.box("s"));
    CHECK(d && d->where() == ScoreDoc::Where::Box && d->box() == r.box("s"));
    CHECK(ScoreDoc::ofBox(r.box("s")) == d);        // the same document
    CHECK(ScoreDoc::ofBox(r.box("o")) == nullptr);
    uint64_t id = tl::addSound(*d, "imp", true, "A", 0.5, 0.1);
    CHECK(id != 0 && scoreOfBox(r.box("s"))->find(id));
    CHECK(edited == 1 && !d->needsSaving());        // kept in the patch: the patch is edited
    r.box("tr")->message(0, {Atom("play")});
    auto y = r.run(44100);
    auto L = above(etest::channel(y, 2, 0), 0.5f);
    CHECK(L.size() == 1 && L[0] == 22050);
    d->undo();
    CHECK(scoreOfBox(r.box("s"))->empty());
    // the box told to add a note: followed, the history cleared
    r.box("s")->message(0, {Atom("add"), Atom("note"), Atom(0.f), Atom(1.f), Atom("Cl"), Atom(60.f)});
    CHECK(d->follow() && d->score().size() == 1 && !d->canUndo() && !d->canRedo());
    CHECK(!d->follow());
    // the patch text applied again (as the code view or the patch's undo does): a new box, followed
    std::string err;
    std::string t = r.p.serialize();
    size_t at = t.find("(obj s score x");
    CHECK(at != std::string::npos);
    if (at != std::string::npos) t.replace(at, 14, "(obj s score y");
    CHECK(r.p.applyText(t, err));
    CHECK(d->follow() && d->box() == r.box("s") && d->score().size() == 1);
    // gone: kept here, said so; edits go on
    r.p.removeBoxes({r.box("s")});
    CHECK(d->follow() && d->boxGone() && d->title().find("not in any open patch") != std::string::npos);
    CHECK(tl::addTrack(*d) == "track 2");
    r.p.collectGarbage(true);
    ScoreDoc::patchEdited = nullptr;
}

// A .score file: edited it is unsaved, saved (atomically) it reads back the same; undone back to the file it is
// not unsaved; opened twice it is one document; a save that cannot be written leaves the file as it was
TEST(scoredoc_file_save_round_trip) {
    fs::path dir = scratch("file");
    std::string path = (dir / "piece.score").string();
    {
        std::ofstream f(path);
        f << "(score \"piece\" (sr 44100) (bpm 120) (meter 4 4) (next-id 2) (row A) (sound 0 1 \"a.wav\" (id 1) (row A)))\n";
    }
    std::string err;
    auto d = ScoreDoc::open(path, err);
    CHECK(d && err.empty());
    CHECK(ScoreDoc::open(path, err) == d);
    CHECK(!d->dirty() && !d->needsSaving());
    CHECK(tl::moveClips(*d, {1}, 0.5));
    CHECK(d->needsSaving() && d->title().find("edited") != std::string::npos);
    CHECK(d->undo() && !d->dirty());
    CHECK(d->redo() && d->dirty());
    CHECK(d->save(err) && !d->dirty());
    std::string back;
    readFile(path, back);
    CHECK(text(*sc::readText(back, path)) == text(*d));
    CHECK(!fs::exists(path + ".tmp"));
    CHECK(d->undo() && d->dirty());
    // a folder that is not there: refused, the file as it was
    CHECK(!d->saveAs((dir / "nope" / "x.score").string(), err) && !err.empty());
    std::string again;
    readFile(path, again);
    CHECK(again == back);
    // a new score saved as a file is that file's document from then on
    auto n = ScoreDoc::make("new");
    tl::addTrack(*n, "A");
    CHECK(n->needsSaving() && !n->save(err));
    CHECK(n->saveAs((dir / "new.score").string(), err) && n->where() == ScoreDoc::Where::File && !n->dirty());
    CHECK(ScoreDoc::open((dir / "new.score").string(), err) == n);
    // untrusted text refused, never a crash
    {
        std::ofstream f(dir / "bad.score");
        f << "(score \"x\" (sound 0 -1 \"a.wav\"))";
    }
    CHECK(!ScoreDoc::open((dir / "bad.score").string(), err) && !err.empty());
    fs::remove_all(dir);
}

// Two views of one score are one document: what one does, the other shows
TEST(scoredoc_two_views_stay_in_sync) {
    Rig r(kPatch);
    auto a = ScoreDoc::ofBox(r.box("s"));
    auto b = ScoreDoc::ofBox(r.box("s"));
    CHECK(a == b);
    uint64_t seen = 0;
    int ta = a->listen([&](unsigned) { seen = a->version(); });
    int tb = b->listen([&](unsigned) { seen = b->version(); });
    tl::addTrack(*a, "A");
    CHECK(seen == b->version() && tl::tracks(b->score()).size() == 1);
    b->undo();
    CHECK(tl::tracks(a->score()).empty());
    a->unlisten(ta);
    b->unlisten(tb);
}

// A placed score opened: its own document (an instance shared by its placements); an edit there makes the parent
// play the change and, embedded, unsaved; Make Unique copies one placement, which the opened one then is not
TEST(scoredoc_placed_score) {
    auto top = ScoreDoc::make("top");
    auto sub = std::make_shared<sc::Score>("gesture", 44100);
    auto n = std::make_shared<sc::Event>();
    n->dur = 0.5;
    n->pitch = 60;
    n->row = "Fl";
    sub->insert(n);
    auto place = [&](double at) {
        auto e = std::make_shared<sc::Event>();
        e->kind = sc::Kind::Score;
        e->sub = sub;
        e->at = at;
        e->dur = 1;
        e->row = "gestures";
        auto ed = top->edit("Place");
        uint64_t id = ed.insert(e);
        ed.commit();
        return id;
    };
    uint64_t p1 = place(0), p2 = place(2), p3 = place(4);
    int which = 0;
    CHECK(tl::instances(top->score(), sub.get(), &which, p2) == 3 && which == 2);
    auto s = ScoreDoc::ofPlacement(top, p2);
    CHECK(s && s->where() == ScoreDoc::Where::Sub && s->parent() == top);
    CHECK(ScoreDoc::ofPlacement(top, p1) == s);     // the same score: the same document
    CHECK(s->chain().size() == 2 && s->chain()[0] == top);
    int topChanges = 0;
    int tok = top->listen([&](unsigned w) { if (w & ScoreDoc::Content) ++topChanges; });
    std::string err;
    CHECK(!top->save(err));                         // no file: refused, nothing changes
    uint64_t v = top->version();
    tl::moveClips(*s, {sub->events()[0]->id}, 0.25);
    CHECK(topChanges == 1 && top->version() > v && top->dirty());
    CHECK(!s->needsSaving());                       // embedded: the parent holds it
    std::vector<sc::Placed> flat;
    sc::flatten(top->score(), flat);
    CHECK(flat.size() == 3 && flat[0].ev.at == 0.25 && flat[1].ev.at == 2.25);
    CHECK(tl::makeUnique(*top, p2));
    CHECK(top->score().find(p2)->sub != sub);
    CHECK(!s->follow() || s->placement() != p2);    // still placed by p1 and p3
    tl::removeClips(*top, {p1, p3});
    CHECK(s->follow() && s->title().find("not placed") != std::string::npos);
    top->unlisten(tok);
}

// Tracks: added, renamed (their events with them), moved, deleted (their events with them); the strip on the row
TEST(timeline_tracks) {
    auto d = ScoreDoc::make("x");
    CHECK(tl::addTrack(*d) == "track 1");
    CHECK(tl::addTrack(*d, "drums") == "drums");
    CHECK(tl::addTrack(*d, "drums").empty());
    uint64_t id = tl::addSound(*d, "/a.wav", false, "drums", 1, 2);
    CHECK(tl::renameTrack(*d, "drums", "perc") && d->score().find(id)->row == "perc");
    CHECK(tl::moveTrack(*d, "perc", -1) && tl::tracks(d->score())[0] == "perc");
    sc::Strip st;
    st.gain = 0.5;
    st.az = 30;
    st.solo = true;
    CHECK(tl::setStrip(*d, "perc", st));
    CHECK(tl::stripOf(d->score(), "perc").gain == 0.5 && tl::stripOf(d->score(), "perc").solo);
    auto back = sc::readText(text(*d), "x");       // kept in the file
    CHECK(sc::stripOf(back->rows[0]).az == 30);
    CHECK(tl::deleteTrack(*d, "perc") && d->score().empty() && tl::tracks(d->score()).size() == 1);
    d->undo();
    CHECK(d->score().size() == 1 && tl::tracks(d->score()).size() == 2);
    // an event on a row the score does not declare is a track too
    auto e = d->edit("x");
    auto ev = std::make_shared<sc::Event>();
    ev->row = "Cl";
    e.insert(ev);
    e.commit();
    CHECK(tl::trackIndex(d->score(), "Cl") == 2);
}

// Clips: moved (in time, to another track; never before 0), trimmed at both ends (a sound's from and to follow),
// split at a time (a sound in two, the regions continuing), placements set, copied and pasted, duplicated, snapped
TEST(timeline_clip_operations) {
    auto d = ScoreDoc::make("x");
    tl::addTrack(*d, "A");
    tl::addTrack(*d, "B");
    uint64_t s = tl::addSound(*d, "/s.wav", false, "A", 1, 2);
    CHECK(tl::moveClips(*d, {s}, 0.5, 1));
    auto e = d->score().find(s);
    CHECK(e->at == 1.5 && e->row == "B");
    CHECK(tl::moveClips(*d, {s}, -10) && d->score().find(s)->at == 0);
    CHECK(tl::trimStart(*d, s, 0.5));
    e = d->score().find(s);
    CHECK(e->at == 0.5 && e->dur == 1.5 && e->from == 0.5 && e->to == 2);
    CHECK(tl::trimEnd(*d, s, 1.5));
    e = d->score().find(s);
    CHECK(e->dur == 1 && e->to == 1.5);
    CHECK(!tl::trimStart(*d, s, -5) || d->score().find(s)->from >= 0);
    e = d->score().find(s);
    CHECK(e->from == 0 && e->at == 0);
    // split: the right half goes on where the left one stops in the source
    auto right = tl::split(*d, {s}, 0.75);
    CHECK(right.size() == 1);
    auto L = d->score().find(s), R = d->score().find(right[0]);
    CHECK(L->dur == 0.75 && L->to == 0.75 && R->at == 0.75 && R->from == 0.75 && R->dur == 0.75 && R->to == 1.5);
    CHECK(tl::split(*d, {s}, 5).empty());           // not under it
    d->undo();
    CHECK(d->score().size() == 1 && d->score().find(s)->dur == 1.5);
    // a placement: stretch, transpose, gain set; one step
    auto inner = std::make_shared<sc::Score>("in", 44100);
    auto pl = std::make_shared<sc::Event>();
    pl->kind = sc::Kind::Score;
    pl->sub = inner;
    pl->dur = 2;
    pl->row = "A";
    auto ed = d->edit("Place");
    uint64_t p = ed.insert(pl);
    ed.commit();
    sc::Event now = *d->score().find(p);
    now.stretch = 0.8;
    now.transpose = 5;
    now.gain = 0.5;
    CHECK(tl::change(*d, now, "Placement"));
    CHECK(d->score().find(p)->transpose == 5 && d->undoName() == "Placement");
    CHECK(!tl::trimStart(*d, p, 1));                // a placement is trimmed from its end
    CHECK(tl::trimEnd(*d, p, 1) && d->score().find(p)->dur == 1);
    // the clipboard
    auto c = tl::copy(d->score(), {s, p});
    CHECK(c.events.size() == 2 && c.origin == 0);
    auto pasted = tl::paste(*d, c, 4, "B");
    CHECK(pasted.size() == 2 && d->score().find(pasted[0])->at == 4 && d->score().find(pasted[0])->row == "B");   // from two tracks: each on its own
    CHECK(d->score().find(pasted[1])->sub == inner);   // a pasted placement is another instance
    auto dup = tl::duplicate(*d, {s});
    CHECK(dup.size() == 1 && d->score().find(dup[0])->at == d->score().find(s)->end());
    CHECK(tl::removeClips(*d, dup) && !d->score().find(dup[0]));
    // snapping at 120 bpm in 4/4: beats of half a second, bars of two
    tl::Snap off = tl::Snap::Off;
    d->edit("t").commit();
    auto h = d->edit("Tempo");
    h.setHeader("x", 120, 4, 4);
    h.commit();
    CHECK(tl::snap(d->score(), 0.7, tl::Snap::Beat) == 0.5);
    CHECK(tl::snap(d->score(), 3.2, tl::Snap::Bar) == 4);
    CHECK(tl::snap(d->score(), 0.7, off) == 0.7);
    CHECK(tl::bbt(d->score(), 2.25) == "2.1.240");
}

// Lanes: declared on the row (they stay without points), points added, moved, the lane removed with its points
TEST(timeline_automation_lanes) {
    auto d = ScoreDoc::make("x");
    tl::addTrack(*d, "A");
    CHECK(tl::addLane(*d, "A", "density", 0, 2));
    CHECK(!tl::addLane(*d, "A", "density"));
    auto ls = tl::lanes(d->score(), "A");
    CHECK(ls.size() == 1 && ls[0].name == "density" && ls[0].hi == 2);
    uint64_t a = tl::addPoint(*d, "A", "density", 1, 0.5);
    uint64_t b = tl::addPoint(*d, "A", "density", 0.5, 1.5);
    auto pts = tl::points(d->score(), "A", "density");
    CHECK(pts.size() == 2 && pts[0]->id == b && tl::pointValue(*pts[1]) == 0.5);
    CHECK(tl::movePoint(*d, a, 2, 1.25));
    CHECK(d->score().find(a)->at == 2 && tl::pointValue(*d->score().find(a)) == 1.25);
    CHECK(!tl::isClip(*d->score().find(a)));
    // a message on the track by another name is a lane too; the file keeps the lanes
    auto back = sc::readText(text(*d), "x");
    CHECK(tl::lanes(*back, "A").size() == 1);
    CHECK(tl::removeLane(*d, "A", "density") && d->score().empty() && tl::lanes(d->score(), "A").empty());
    d->undo();
    CHECK(d->score().size() == 2 && tl::lanes(d->score(), "A").size() == 1);
}

// Heights: a track's and a lane's, kept on the row (so in the file), one undo step for several; a lane known only by
// its points gets a height too (declared then); the same height again is nothing to do; adding a lane keeps them
TEST(timeline_heights) {
    auto d = ScoreDoc::make("x");
    tl::addTrack(*d, "A");
    tl::addTrack(*d, "B");
    tl::addLane(*d, "A", "density", 0, 2);
    tl::addPoint(*d, "B", "cutoff", 1, 0.5);                       // a lane by its points only
    CHECK(tl::trackHeight(d->score(), "A") == 0 && tl::lanes(d->score(), "A")[0].h == 0);
    size_t steps = d->historySteps();
    CHECK(tl::setHeights(*d, {{"A", "", 90}, {"A", "density", 25}, {"B", "cutoff", 30}}));
    CHECK(d->historySteps() == steps + 1);
    CHECK(tl::trackHeight(d->score(), "A") == 90 && tl::trackHeight(d->score(), "B") == 0);
    CHECK(tl::lanes(d->score(), "A")[0].h == 25 && tl::lanes(d->score(), "B")[0].h == 30);
    CHECK(tl::lanes(d->score(), "A")[0].hi == 2);                  // its range kept
    CHECK(!tl::setHeights(*d, {{"A", "", 90}}));                     // the same
    CHECK(!tl::setHeights(*d, {{"C", "", 40}}) && !tl::setHeights(*d, {{"A", "nope", 40}}));
    auto back = sc::readText(text(*d), "x");                         // the file keeps them
    CHECK(tl::trackHeight(*back, "A") == 90 && tl::lanes(*back, "A")[0].h == 25 && tl::lanes(*back, "B")[0].h == 30);
    CHECK(tl::addLane(*d, "A", "gain"));                             // another lane: the heights stay
    CHECK(tl::lanes(d->score(), "A")[0].h == 25 && tl::lanes(d->score(), "A")[1].h == 0);
    CHECK(tl::setHeights(*d, {{"A", "", 0}}) && tl::trackHeight(d->score(), "A") == 0);   // 0: the default, nothing kept
    CHECK(text(*d).find("height") == std::string::npos);
    d->undo();
    d->undo();
    d->undo();
    CHECK(tl::trackHeight(d->score(), "A") == 0 && tl::lanes(d->score(), "A")[0].h == 0);
}

// The transport of a score's window: Play plays from the playhead, located first, also when the patch's transport
// already runs past the score's end (the score not playing then, the patch's clock left alone by the timer); a
// selection made after the playhead was placed: from its start; a play started here stops at the score's end, the
// playhead back where it started; looping, it goes on
TEST(score_playback_always_plays) {
    Rig r(kPatch);
    impulse(r, "b");
    auto d = ScoreDoc::ofBox(r.box("s"));
    uint64_t a = tl::addSound(*d, "imp", true, "A", 0.5, 0.5);     // the score ends at 1 s
    ScorePlayback pb(d, &r.p);
    auto t = transportOf(&r.p);
    CHECK(pb.clock() == &r.p);
    pb.setPlayhead(0.25);
    CHECK(!pb.playing() && !pb.running() && pb.position() == 0.25);
    CHECK(pb.play() && pb.playing() && t->playing() && pb.startedHere());
    r.run(4410);
    CHECK(std::fabs(pb.position() - 0.35) < 0.01);
    pb.stop();
    CHECK(!t->playing() && std::fabs(pb.playhead() - 0.35) < 0.01);
    t->locate(&r.p, 5);                                               // the patch's clock, past the score's end
    t->play(&r.p);
    CHECK(pb.running() && !pb.playing() && pb.position() == pb.playhead());
    pb.tick();
    CHECK(t->playing());                                              // not started here: left alone
    CHECK(pb.play() && pb.playing() && std::fabs(pb.position() - pb.playhead()) < 0.01);   // located: it plays
    d->clearSelection();                                              // (adding selected it)
    d->select({a});                                                   // selected after the playhead was placed
    CHECK(pb.startPoint() == 0.5);
    pb.setPlayhead(0.9);
    CHECK(pb.startPoint() == 0.9);
    CHECK(pb.play());
    r.run(4410 * 2);                                                  // past the end
    pb.tick();
    CHECK(!t->playing() && !pb.playing() && pb.playhead() == 0.9 && !pb.startedHere());
    pb.setLoopRange(0.5, 1);
    pb.setLoop(true);
    CHECK(pb.loop() && pb.play());
    r.run(44100);
    pb.tick();
    CHECK(t->playing() && pb.playing());
    pb.stop();
    CHECK(!t->playing());
}

// The tracks' strips in playback: a muted track is silent, a soloed one alone sounds, gain scales, pan places;
// a sound's fades are played
TEST(timeline_tracks_play_with_their_strips) {
    Rig r(kPatch);
    impulse(r, "b");
    auto d = ScoreDoc::ofBox(r.box("s"));
    tl::addTrack(*d, "A");
    tl::addTrack(*d, "B");
    tl::addSound(*d, "imp", true, "A", 0.25, 0.1);
    tl::addSound(*d, "imp", true, "B", 0.5, 0.1);
    auto play = [&](std::vector<long>& left, std::vector<long>& right, float level = 0.1f) {
        r.box("tr")->message(0, {Atom("locate"), Atom(1.f)});
        r.box("tr")->message(0, {Atom("play")});
        auto y = r.run(44100);
        r.box("tr")->message(0, {Atom("stop")});
        left = above(etest::channel(y, 3, 0), level);
        right = above(etest::channel(y, 3, 1), level);
        return y;
    };
    r.ch = 3;
    std::vector<long> L, R;
    play(L, R);
    CHECK(L.size() == 2 && R.size() == 2);
    sc::Strip m;
    m.mute = true;
    tl::setStrip(*d, "A", m);
    play(L, R);
    CHECK(L.size() == 1 && L[0] == 22050);
    tl::setStrip(*d, "A", sc::Strip{});
    sc::Strip s;
    s.solo = true;
    tl::setStrip(*d, "A", s);
    play(L, R);
    CHECK(L.size() == 1 && L[0] == 11025);
    // gain and pan: A hard left (90 degrees), at half its gain
    s.solo = false;
    s.gain = 0.5;
    s.az = 90;
    tl::setStrip(*d, "A", s);
    auto y = play(L, R, 0.01f);
    auto left = etest::channel(y, 3, 0), right = etest::channel(y, 3, 1);
    CHECK(std::fabs(left[11025] - 0.5f) < 1e-4f && std::fabs(right[11025]) < 1e-4f);
    CHECK(std::fabs(left[22050] - right[22050]) < 1e-4f);    // B in the middle
    // fades: a constant sound rises over its fade-in and falls over its fade-out
    constant(r, "k", 44100);
    tl::deleteTrack(*d, "A");
    tl::deleteTrack(*d, "B");
    uint64_t id = tl::addSound(*d, "one", true, "C", 0, 0.5);
    sc::Event now = *d->score().find(id);
    sc::setField(now.extra, "fade-in", sc::Datum::number(0.1));
    sc::setField(now.extra, "fade-out", sc::Datum::number(0.2));
    tl::change(*d, now, "Fades");
    y = play(L, R, 0.f);
    left = etest::channel(y, 3, 0);
    float g = (float)std::sqrt(0.5);
    CHECK(left[0] == 0.f && std::fabs(left[2205] - 0.5f * g) < 0.01f && std::fabs(left[8820] - g) < 1e-4f);
    CHECK(std::fabs(left[22050 - 4410] - 0.5f * g) < 0.01f && std::fabs(left[22050 + 10]) < 1e-6f);
}

// An automation lane plays as the messages it is: each point at its sample, to the receive of its name and out of
// the messages outlet, whatever the track's strip says
TEST(timeline_automation_lands_on_its_sample) {
    Rig r(kPatch);
    r.ch = 3;
    auto d = ScoreDoc::ofBox(r.box("s"));
    tl::addTrack(*d, "A");
    tl::addLane(*d, "A", "density");
    tl::addPoint(*d, "A", "density", 0.25, 0.3);
    tl::addPoint(*d, "A", "density", 0.7071, 0.9);
    sc::Strip m;
    m.mute = true;
    tl::setStrip(*d, "A", m);
    r.run(1000);
    r.box("tr")->message(0, {Atom("play")});
    auto y = r.run(44100);
    auto clicks = above(etest::channel(y, 3, 2), 0.5f);
    CHECK(clicks.size() == 2 && clicks[0] == 11025 && clicks[1] == 31183);
    CHECK(r.printedSomething("lane: 0.3") && r.printedSomething("lane: 0.9"));
}

// The timeline's edits while the audio thread plays the score: moves, trims, splits, strips, undo and redo, a lane's
// points, the box told to clear and followed; nothing torn (ThreadSanitizer), nothing allocated there
TEST(timeline_edited_while_audio_runs) {
    Rig r(kPatch);
    impulse(r, "b");
    auto d = ScoreDoc::ofBox(r.box("s"));
    tl::addTrack(*d, "A");
    tl::addTrack(*d, "B");
    r.box("tr")->message(0, {Atom("loop"), Atom(1.f), Atom(2.f)});
    r.box("tr")->message(0, {Atom("play")});
    std::atomic<bool> stop{false};
    std::atomic<long> blocks{0};
    std::thread audio([&] {
        std::vector<float> buf(3 * 64);
        while (!stop) {
            r.p.process(nullptr, 0, buf.data(), 3, 64);
            blocks.fetch_add(1);
        }
    });
    std::mt19937 rng(3);
    for (int k = 0; k < 300; ++k) {
        uint64_t id = tl::addSound(*d, "imp", true, k % 2 ? "A" : "B", (rng() % 100) / 100.0, 0.2);
        tl::moveClips(*d, {id}, 0.01, k % 3 == 0 ? 1 : 0);
        tl::trimEnd(*d, id, d->score().find(id)->at + 0.1);
        tl::split(*d, {id}, d->score().find(id)->at + 0.05);
        if (k % 5 == 0) {
            sc::Strip st;
            st.mute = k % 10 == 0;
            st.gain = 0.5;
            tl::setStrip(*d, "A", st);
        }
        if (k % 7 == 0) tl::addPoint(*d, "B", "density", (rng() % 100) / 100.0, 0.5);
        if (k % 4 == 0) { d->undo(); d->undo(); d->redo(); }
        if (k % 50 == 49) {
            r.box("s")->message(0, {Atom("clear")});
            d->follow();
        }
        r.p.runDue(r.p.time() + 0.01);
        r.p.collectGarbage();
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    stop = true;
    audio.join();
    r.p.collectGarbage(true);
    CHECK(blocks.load() > 0);
    CHECK(!r.printedSomething("score:"));
}

// A score of its own (a file, a new one) plays on a score box of its own, beside the patches, removed with it
TEST(scoredoc_plays_without_a_patch) {
    Rig r("(patch (obj b buffer~ imp 100))");
    impulse(r, "b");
    size_t extras = r.p.extras.size();
    {
        auto d = ScoreDoc::make("x");
        tl::addSound(*d, "imp", true, "A", 0.5, 0.1);
        Box* p = d->player(&r.p);
        CHECK(p && r.p.extras.size() == extras + 1 && scoreOfBox(p) == d->scorePtr());
        CHECK(d->player(&r.p) == p);
        transportOf(&r.p)->play(&r.p);
        auto y = r.run(44100);
        auto L = above(etest::channel(y, 2, 0), 0.5f);
        CHECK(L.size() == 1 && L[0] == 22050);
        transportOf(&r.p)->stop(&r.p);
        tl::moveClips(*d, d->score().events().empty() ? std::vector<uint64_t>{} : std::vector<uint64_t>{d->score().events()[0]->id}, 0.25);
        CHECK(scoreOfBox(p)->events()[0]->at == 0.75);
    }
    CHECK(r.p.extras.size() == extras);
    r.p.collectGarbage(true);
}

// The render: the score's sounds through their tracks' strips, offline, into a WAV file
TEST(timeline_render_to_wav) {
    fs::path dir = scratch("render");
    // a click file next to the score
    SampleBuffer click;
    click.channels = 1;
    click.sr = 44100;
    click.frames = 100;
    click.data.assign(100, 0.f);
    click.data[0] = 1.f;
    std::string err;
    CHECK(writeWavFile((dir / "click.wav").string(), click, err, 24));
    auto d = ScoreDoc::make("x");
    CHECK(d->saveAs((dir / "r.score").string(), err));
    tl::addSound(*d, (dir / "click.wav").string(), false, "A", 0.5, 0.002);
    tl::addSound(*d, (dir / "click.wav").string(), false, "B", 1, 0.002);
    CHECK(d->score().events()[0]->source == "click.wav");   // kept relative to the score
    sc::Strip m;
    m.mute = true;
    tl::setStrip(*d, "B", m);
    bool sync = jobsSynchronous;
    jobsSynchronous = true;
    bool done = false, ok = false;
    {
        ScoreRender render;
        std::string out = (dir / "out.wav").string();
        CHECK(render.start(d->score(), d->folder(), out, 2, 44100, [&](bool k, const std::string&) { done = true; ok = k; }, err));
    }
    jobsSynchronous = sync;
    CHECK(done && ok);
    SampleBuffer got;
    CHECK(decodeAudioFile((dir / "out.wav").string(), got, err));
    CHECK(got.channels == 2 && got.frames > 44100);
    std::vector<long> hits;
    for (long f = 0; f < got.frames; ++f)
        if (got.at(f, 0) > 0.5f) hits.push_back(f);
    CHECK(hits.size() == 1 && hits[0] == 22050);
    ScoreRender empty;
    CHECK(!empty.start(sc::Score("e", 44100), "", (dir / "e.wav").string(), 2, 44100, nullptr, err));
    fs::remove_all(dir);
}
