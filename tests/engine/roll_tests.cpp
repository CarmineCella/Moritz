// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// roll_tests.cpp — the roll's edits on a score box's document (roll_edit.h, score_doc.h): each intent exact, undo and
// redo, scores placed inside (every instance follows), the mouse and the keys of the roll (roll.h) turned into those
// edits, the roll and the timeline as views of one document (one history, one selection), the cents shown and edited,
// the box's messages (select, transpose, quantize...), a note that plays its own sound, edits while the audio thread
// plays, and a box that goes while its roll is open.

#include "rig.h"
#include "roll.h"
#include "score_doc.h"
#include "timeline_ops.h"
#include "transport.h"

#include "music/score.h"
#include "music/score_text.h"

#include <FL/Enumerations.H>

#include <atomic>
#include <filesystem>
#include <cmath>
#include <thread>

using namespace moritz;
using etest::Rig;
namespace sc = musil::score;
namespace tl = moritz::timeline;

namespace {

std::shared_ptr<sc::Event> note(double at, double dur, double pitch, const char* row = "Fl", const char* dyn = "mf") {
    auto e = std::make_shared<sc::Event>();
    e->kind = sc::Kind::Note;
    e->at = at;
    e->dur = dur;
    e->pitch = pitch;
    e->row = row;
    e->dyn = dyn;
    return e;
}
// three notes on a flute: C4 at 0, E4 at 0.5, G4 at 1 (60 bpm, 4/4: the grid of 1/16 is 0.25 s)
std::shared_ptr<sc::Score> three() {
    auto s = std::make_shared<sc::Score>("t", 44100);
    s->insert(note(0, 0.5, 60));
    s->insert(note(0.5, 0.5, 64));
    s->insert(note(1, 0.5, 67, "Fl", "f"));
    return s;
}
void give(Rig& r, const char* id, std::shared_ptr<sc::Score> s) {
    r.box(id)->message(0, {Atom("score"), Atom(shareScore(s))});
}
// the roll's target on a box's document (as the roll's window makes it)
std::shared_ptr<ScoreDocTarget> targetOf(Rig& r, const char* id) { return makeRollTarget(ScoreDoc::ofBox(r.box(id))); }
std::shared_ptr<const sc::Score> now(Rig& r, const char* id) { return scoreOfBox(r.box(id)); }
const sc::Event& ev(Rig& r, const char* id, uint64_t eid) { return *now(r, id)->find(eid); }
// the score as text, but for the next id (ids are never given back: an undo does not lower it)
std::string text(const sc::Score& s) {
    std::string t = sc::toText(s);
    size_t k = t.find("(next-id ");
    if (k != std::string::npos) t.erase(k, t.find(')', k) - k + 1);
    return t;
}

const char* kPatch = "(patch (obj s score t 1) (obj o dac~ 1) (obj tr transport 60 4 4) (connect s 0 o 0))";

}  // namespace

// Each intent changes exactly what it names, once, as one undo step; undo and redo give back the same scores
TEST(roll_intents_on_a_score_box) {
    Rig r(kPatch);
    give(r, "s", three());
    auto t = targetOf(r, "s");
    std::string t0 = text(*t->score());
    uint64_t g0 = t->generation();
    CHECK(t->moveEvents({}, {1}, 0.5, 2));
    CHECK(ev(r, "s", 1).at == 0.5 && ev(r, "s", 1).pitch == 62);
    CHECK(ev(r, "s", 2).at == 0.5 && ev(r, "s", 2).pitch == 64);       // the others as they were
    CHECK(t->generation() != g0);
    CHECK(t->resizeEvents({}, {2, 3}, 0.25));
    CHECK(ev(r, "s", 2).dur == 0.75 && ev(r, "s", 3).dur == 0.75);
    Ids copies = t->copyEvents({}, {3}, 1, -12);
    CHECK(copies.size() == 1 && copies[0] == 4);
    CHECK(ev(r, "s", 4).at == 2 && ev(r, "s", 4).pitch == 55 && ev(r, "s", 4).dyn == "f" && ev(r, "s", 3).at == 1);
    sc::Event n;
    n.kind = sc::Kind::Note; n.at = 3; n.dur = 0.25; n.pitch = 72; n.row = "Vc";
    CHECK(t->insertEvent({}, n) == 5);
    CHECK(t->setField({}, {5}, "pitch", sc::Datum::symbol("G5")) && ev(r, "s", 5).pitch == 79);
    CHECK(t->setField({}, {5}, "dyn", sc::Datum::symbol("pp")) && ev(r, "s", 5).dyn == "pp");
    CHECK(!t->setField({}, {5}, "dur", sc::Datum::symbol("long")));   // a word where a number is wanted: nothing
    CHECK(!t->lastError.empty());
    CHECK(t->removeEvents({}, {4, 5}) && now(r, "s")->size() == 3);
    ScoreTransform tp;
    tp.kind = ScoreTransform::Transpose;
    tp.a = 12;
    CHECK(t->transform({}, {}, tp));
    CHECK(ev(r, "s", 1).pitch == 74 && ev(r, "s", 2).pitch == 76 && ev(r, "s", 3).pitch == 79);
    std::string t8 = text(*t->score());
    int steps = 0;
    while (t->undo()) ++steps;
    CHECK(steps == 8);
    CHECK(text(*t->score()) == t0);
    while (t->redo()) --steps;
    CHECK(steps == 0);
    CHECK(text(*t->score()) == t8);
    CHECK(!t->moveEvents({}, {}, 1, 0));                          // nothing named: nothing done, no step
    CHECK(!t->moveEvents({}, {99}, 1, 0) || now(r, "s")->find(99) == nullptr);
}

// A score the target hands out is a copy (a view keeps the one it has while the document changes); a message to the
// box changes the score from outside: the document follows it and its history starts again
TEST(roll_scores_handed_out_are_copies) {
    Rig r(kPatch);
    give(r, "s", three());
    auto t = targetOf(r, "s");
    auto before = t->score();
    std::string text = sc::toText(*before);
    CHECK(t->moveEvents({}, {1, 2, 3}, 1, 1));
    CHECK(sc::toText(*before) == text);
    CHECK(t->score() != before && t->score()->find(1)->at == 1 && t->canUndo());
    r.box("s")->message(0, {Atom("clear")});
    r.box("s")->message(0, {Atom("add"), Atom(1.f), Atom("Fl"), Atom("C5")});
    CHECK(sc::toText(*before) == text);
    CHECK(now(r, "s")->size() == 1 && t->score()->size() == 1);
    CHECK(!t->canUndo());
    // an edit, then a message before any view looked again: Undo does not put back the score the message replaced
    uint64_t id = now(r, "s")->events()[0]->id;
    CHECK(t->moveEvents({}, {id}, 1, 0));
    r.box("s")->message(0, {Atom("transpose"), Atom(2.f)});
    CHECK(!t->undo() && now(r, "s")->find(id)->pitch == 74 && now(r, "s")->find(id)->at == 2);
}

// Scores placed inside: an edit of the score at a path changes it in every place it is placed (an instance), the
// top copied on the way down, the score before left as it was
TEST(roll_edits_a_score_placed_inside) {
    Rig r(kPatch);
    auto inner = std::make_shared<sc::Score>("texture", 44100);
    inner->insert(note(0, 0.25, 72));
    inner->insert(note(0.25, 0.25, 74));
    auto top = std::make_shared<sc::Score>("piece", 44100);
    top->insert(note(0, 1, 60, "Vn"));
    auto place = [&](double at, double tr) {
        auto e = std::make_shared<sc::Event>();
        e->kind = sc::Kind::Score;
        e->sub = inner;
        e->at = at;
        e->dur = 1;
        e->stretch = 1.5;
        e->transpose = tr;
        e->row = "Vn";
        return e;
    };
    top->insert(place(1, 7));
    top->insert(place(3, -5));
    give(r, "s", top);
    auto t = targetOf(r, "s");
    uint64_t g0 = t->generation();
    CHECK(t->moveEvents({2}, {1}, 0.125, 1));
    auto s = now(r, "s");
    CHECK(s->find(2)->sub == s->find(3)->sub);                          // still one score, placed twice
    CHECK(s->find(2)->sub->find(1)->at == 0.125 && s->find(2)->sub->find(1)->pitch == 73);
    CHECK(t->generation() != g0);                                        // the top's views see it
    // the score placed is a document of its own (the one the timeline opens for it), with the edit in its history
    auto sub = ScoreDoc::ofPlacement(t->document(), 2);
    CHECK(sub && sub->canUndo() && sub->undoName() == "move" && !t->document()->canUndo());
    CHECK(ScoreDoc::ofPlacement(t->document(), 3) == sub);               // the other placement: the same score
    // the roll shows the score inside: its time from the transport's through the placement
    RollCore core(t);
    core.setSize(600, 300);
    core.enter(3);
    CHECK(core.path == ScorePath{3});
    CHECK(std::fabs(core.shownTime(3.3) - 0.2) < 1e-12);
    CHECK(std::isnan(core.shownTime(0.5)));
    CHECK(core.crumbs().size() == 2 && core.crumbs()[1] == "texture");
    CHECK(t->canUndo());                                     // inside it: its history
    core.up();
    CHECK(core.path.empty() && core.selection.count(3));
    CHECK(!t->canUndo());                                    // at the top: the top's (nothing)
    t->focus({2});
    CHECK(t->undo() && now(r, "s")->find(2)->sub->find(1)->at == 0);   // and the box plays it
    CHECK(!t->moveEvents({9}, {1}, 0.125, 0) && !t->lastError.empty());   // no such placement
}

// The mouse: a note dragged by half a second and a step up the staff, its end dragged, Alt-drag copies, a rectangle
// selects, Draw adds a note on the grid, Erase takes one out; the keys: Delete, the arrows
TEST(roll_mouse_and_keys) {
    Rig r(kPatch);
    give(r, "s", three());
    auto t = targetOf(r, "s");
    RollCore c(t);
    c.setSize(678, 400);
    c.fit();
    CHECK(std::fabs(c.snapSeconds() - 0.25) < 1e-12);
    double x, y, w, h;
    CHECK(c.eventBox(1, x, y, w, h));
    double hx = x + h / 2, hy = y + h / 2;                               // the head of C4
    const auto& row = c.rows()[0];
    CHECK(row.staff && row.spec.name == "Fl" && row.spec.clef == "treble");
    CHECK(c.hit(hx, hy).id == 1 && !c.hit(hx, hy).end);
    c.mouse(FL_PUSH, hx, hy, 0, 0);
    c.mouse(FL_DRAG, hx + 0.3 * c.pps, hy - row.sp / 2, 0, 0);          // 0.3 s: snapped to 0.25
    c.mouse(FL_DRAG, hx + 0.5 * c.pps, hy - row.sp / 2, 0, 0);
    c.mouse(FL_RELEASE, hx + 0.5 * c.pps, hy - row.sp / 2, 0, 0);
    CHECK(ev(r, "s", 1).at == 0.5 && ev(r, "s", 1).pitch == 62);        // C4 -> D4, at the grid
    CHECK(c.selection == std::set<uint64_t>{1});
    CHECK(t->undo() && ev(r, "s", 1).at == 0 && ev(r, "s", 1).pitch == 60);
    // the end of E4: a quarter of a second longer
    CHECK(c.eventBox(2, x, y, w, h));
    double ex = x + w - 1, ey = y + h / 2;
    CHECK(c.hit(ex, ey).id == 2 && c.hit(ex, ey).end);
    c.mouse(FL_PUSH, ex, ey, 0, 0);
    c.mouse(FL_DRAG, ex + 0.25 * c.pps, ey, 0, 0);
    c.mouse(FL_RELEASE, ex + 0.25 * c.pps, ey, 0, 0);
    CHECK(ev(r, "s", 2).dur == 0.75 && ev(r, "s", 2).at == 0.5);
    // Alt-drag: a copy a second later, selected
    CHECK(c.eventBox(3, x, y, w, h));
    c.mouse(FL_PUSH, x + h / 2, y + h / 2, FL_ALT, 0);
    c.mouse(FL_DRAG, x + h / 2 + c.pps, y + h / 2, FL_ALT, 0);
    c.mouse(FL_RELEASE, x + h / 2 + c.pps, y + h / 2, FL_ALT, 0);
    CHECK(now(r, "s")->size() == 4 && ev(r, "s", 3).at == 1 && ev(r, "s", 4).at == 2 && ev(r, "s", 4).pitch == 67);
    CHECK(c.selection == std::set<uint64_t>{4});
    // a rectangle around the first two
    double x1, y1, w1, h1;
    c.eventBox(1, x, y, w, h);
    c.eventBox(2, x1, y1, w1, h1);
    double top = std::min(y, y1) - 4, bottom = std::max(y + h, y1 + h1) + 4;
    c.mouse(FL_PUSH, c.xOf(0) - 20 + 22, top - 30, 0, 0);               // empty space above the notes
    c.mouse(FL_DRAG, c.xOf(0.75), bottom, 0, 0);
    c.mouse(FL_RELEASE, c.xOf(0.75), bottom, 0, 0);
    CHECK((c.selection == std::set<uint64_t>{1, 2}));
    // the keys: right a grid step, up a semitone, Delete
    CHECK(c.key(FL_Right, 0) && ev(r, "s", 1).at == 0.25 && ev(r, "s", 2).at == 0.75);
    CHECK(c.key(FL_Up, 0) && ev(r, "s", 1).pitch == 61 && ev(r, "s", 2).pitch == 65);
    CHECK(c.key(FL_Up, FL_SHIFT) && ev(r, "s", 1).pitch == 73);
    CHECK(c.key(FL_Delete, 0) && now(r, "s")->size() == 2 && c.selection.empty());
    // Draw: a note where the mouse is (A4 on the treble staff: two spaces above the bottom line... the second space)
    c.tool = RollCore::Tool::Draw;
    double yA4 = c.yOfPitch(0, 69);
    c.mouse(FL_PUSH, c.xOf(2.6), yA4, 0, 0);
    c.mouse(FL_DRAG, c.xOf(3.1), yA4, 0, 0);
    c.mouse(FL_RELEASE, c.xOf(3.1), yA4, 0, 0);
    auto s = now(r, "s");
    CHECK(s->size() == 3);
    const sc::Event* drawn = s->find(5).get();
    CHECK(drawn && drawn->at == 2.5 && drawn->dur == 0.5 && drawn->pitch == 69 && drawn->row == "Fl");
    // Erase: a click on it
    c.tool = RollCore::Tool::Erase;
    c.eventBox(5, x, y, w, h);
    c.mouse(FL_PUSH, x + h / 2, y + h / 2, 0, 0);
    c.mouse(FL_RELEASE, x + h / 2, y + h / 2, 0, 0);
    CHECK(now(r, "s")->size() == 2 && !now(r, "s")->find(5));
    // quantize (Q) puts an onset off the grid back on it
    c.tool = RollCore::Tool::Select;
    t->setField({}, {3}, "at", sc::Datum::number(1.1));
    c.selection = {3};
    CHECK(c.key('q', 0) && ev(r, "s", 3).at == 1);
}

// The piano roll: a note dragged up two keys is two semitones higher
TEST(roll_piano_mode) {
    Rig r(kPatch);
    give(r, "s", three());
    auto t = targetOf(r, "s");
    RollCore c(t);
    c.mode = RollCore::Mode::Piano;
    c.setSize(678, 400);
    c.fit();
    double x, y, w, h;
    CHECK(c.eventBox(2, x, y, w, h));
    double key = c.yOfPitch(0, 64) - c.yOfPitch(0, 65);
    CHECK(key > 0);
    c.mouse(FL_PUSH, x + 4, y + h / 2, 0, 0);
    c.mouse(FL_DRAG, x + 4, y + h / 2 - 2 * key, 0, 0);
    c.mouse(FL_RELEASE, x + 4, y + h / 2 - 2 * key, 0, 0);
    CHECK(ev(r, "s", 2).pitch == 66 && ev(r, "s", 2).at == 0.5);
}

// The box's messages: select, then transformations of the selection (all of it when none), add without a kind or a
// duration, clear; none of them changes the scores handed out before
TEST(score_box_transform_messages) {
    Rig r(kPatch);
    give(r, "s", three());
    Box* b = r.box("s");
    b->message(0, {Atom("select"), Atom(1.f), Atom(3.f)});
    b->message(0, {Atom("transpose"), Atom(-2.f)});
    CHECK(ev(r, "s", 1).pitch == 58 && ev(r, "s", 2).pitch == 64 && ev(r, "s", 3).pitch == 65);
    b->message(0, {Atom("select"), Atom("none")});
    b->message(0, {Atom("shift"), Atom(0.5f)});
    CHECK(ev(r, "s", 1).at == 0.5 && ev(r, "s", 3).at == 1.5);
    b->message(0, {Atom("augment"), Atom(2.f)});                        // from the first onset
    CHECK(ev(r, "s", 1).at == 0.5 && ev(r, "s", 2).at == 1.5 && ev(r, "s", 3).at == 2.5 && ev(r, "s", 3).dur == 1);
    b->message(0, {Atom("diminish"), Atom(2.f)});
    CHECK(ev(r, "s", 3).at == 1.5 && ev(r, "s", 3).dur == 0.5);
    b->message(0, {Atom("invert"), Atom("C4")});
    CHECK(ev(r, "s", 1).pitch == 62 && ev(r, "s", 2).pitch == 56);
    b->message(0, {Atom("invert"), Atom(60.f)});
    CHECK(ev(r, "s", 1).pitch == 58);
    b->message(0, {Atom("retrograde")});
    CHECK(ev(r, "s", 1).at == 1.5 && ev(r, "s", 3).at == 0.5);
    b->message(0, {Atom("select"), Atom("row"), Atom("Fl")});
    b->message(0, {Atom("add"), Atom(0.3f), Atom("Vc"), Atom("C3")});   // no kind, no duration: a note a beat long
    auto s = now(r, "s");
    CHECK(s->size() == 4 && s->find(4)->row == "Vc" && s->find(4)->pitch == 48 && s->find(4)->dur == 1 && s->find(4)->at == 0.3f);
    b->message(0, {Atom("add"), Atom("note"), Atom(0.3f), Atom(0.5f), Atom("Vc"), Atom("D3"), Atom("ff")});
    CHECK(now(r, "s")->find(5)->dur == 0.5f && now(r, "s")->find(5)->dyn == "ff");
    b->message(0, {Atom("select"), Atom("none")});
    b->message(0, {Atom("quantize"), Atom(0.5f)});
    CHECK(now(r, "s")->find(4)->at == 0.5);
    b->message(0, {Atom("thin"), Atom(0.f)});
    CHECK(now(r, "s")->empty());
    give(r, "s", three());
    b->message(0, {Atom("density"), Atom(1.f), Atom(0.f)});
    CHECK(now(r, "s")->size() == 2 && !now(r, "s")->find(3));
    b->message(0, {Atom("humanize"), Atom(10.f), Atom(0.f), Atom(4.f)});
    double moved = std::fabs(ev(r, "s", 1).at - 0) + std::fabs(ev(r, "s", 2).at - 0.5);
    CHECK(moved > 0 && moved <= 0.0200001);
    CHECK(!r.printedSomething("score:"));
    b->message(0, {Atom("augment"), Atom(0.f)});
    CHECK(r.printedSomething("score: augment by a factor > 0"));
}

// A note with a sound of its own plays it: on its onset, as the same region of the file as a sound event would, and
// an octave higher at twice the speed
TEST(score_note_plays_its_sound) {
    std::string wav = std::filesystem::absolute("data/anechoic1.wav").string();
    auto play = [&](std::shared_ptr<sc::Event> e) {
        Rig r(kPatch);
        auto s = std::make_shared<sc::Score>("t", 44100);
        s->insert(e);
        give(r, "s", s);
        r.box("tr")->message(0, {Atom("play")});
        r.ch = 1;
        return r.run(22050);
    };
    auto snd = std::make_shared<sc::Event>();
    snd->kind = sc::Kind::Sound; snd->source = wav; snd->at = 0.1; snd->dur = 0.3;
    auto n = note(0.1, 0.3, 60);
    sc::setField(n->extra, "sound", sc::Datum::string(wav));
    sc::setField(n->extra, "sound-pitch", sc::Datum::number(60));
    auto a = play(snd), b = play(n);
    CHECK(a.size() == b.size());
    double diff = 0, energy = 0;
    for (size_t k = 0; k < a.size(); ++k) { diff += std::fabs(a[k] - b[k]); energy += std::fabs(a[k]); }
    CHECK(energy > 1 && diff == 0);
    bool silentBefore = true;
    for (size_t k = 0; k < 4410; ++k) silentBefore = silentBefore && b[k] == 0;
    CHECK(silentBefore);
    auto up = note(0.1, 0.3, 72);
    sc::setField(up->extra, "sound", sc::Datum::string(wav));
    sc::setField(up->extra, "sound-pitch", sc::Datum::number(60));
    auto c = play(up);
    // an octave up: sample k of it is sample 2k of the sound (the first 0.1 s of it, before the fade)
    double err = 0;
    for (size_t k = 4410; k < 4410 + 2000; ++k) err += std::fabs(c[k] - a[4410 + 2 * (k - 4410)]);
    CHECK(err < 1e-3);
}

// Edits through the roll's target while the audio thread plays the score (the plan replaced under it)
TEST(roll_edits_while_audio_runs) {
    Rig r(kPatch);
    auto s = std::make_shared<sc::Score>("t", 44100);
    for (int j = 0; j < 40; ++j) s->insert(note(j * 0.05, 0.05, 60 + j % 12));
    give(r, "s", s);
    auto t = targetOf(r, "s");
    r.box("tr")->message(0, {Atom("loop"), Atom(1.f), Atom(2.f)});
    r.box("tr")->message(0, {Atom("play")});
    std::atomic<bool> stop{false};
    std::thread audio([&] {
        std::vector<float> buf(64);
        while (!stop) r.p.process(nullptr, 0, buf.data(), 1, 64);
    });
    RollCore c(t);
    c.setSize(600, 300);
    for (int k = 0; k < 300; ++k) {
        t->moveEvents({}, {(uint64_t)(1 + k % 40)}, 0.01, k % 2 ? 1 : -1);
        if (k % 5 == 0) t->undo();
        if (k % 9 == 0) { ScoreTransform q; q.kind = ScoreTransform::Quantize; q.a = 0.05; q.b = 1; t->transform({}, {}, q); }
        c.hit(100, 100);
        r.p.runDue(r.p.time() + 0.01);
        r.p.collectGarbage();
    }
    stop = true;
    audio.join();
    r.p.collectGarbage(true);
    CHECK(now(r, "s")->size() == 40);
    CHECK(!r.printedSomething("score:"));
}

// The box goes while its roll is open: the document keeps the score (as the timeline does), its edits stay there and
// reach no box; the patch destroyed, it still holds it
TEST(roll_outlives_its_box) {
    auto r = std::make_unique<Rig>(kPatch);
    give(*r, "s", three());
    auto t = targetOf(*r, "s");
    RollCore c(t);
    c.setSize(600, 300);
    CHECK(t->moveEvents({}, {1}, 1, 0) && t->patch() == &r->p);
    Box* box = r->box("s");
    r->p.removeBoxes({box});
    CHECK(t->document()->boxGone() && t->patch() == nullptr);
    CHECK(t->moveEvents({}, {1}, 1, 0) && t->score()->find(1)->at == 2);
    r->p.collectGarbage(true);                               // and gone
    CHECK(t->score() && t->score()->find(1)->at == 2);
    CHECK(t->undo() && t->score()->find(1)->at == 1);
    c.mouse(FL_PUSH, 200, 100, 0, 0);
    c.mouse(FL_RELEASE, 200, 100, 0, 0);
    double x, y, w, h;
    CHECK(c.eventBox(1, x, y, w, h));
    r.reset();
    CHECK(t->score()->size() == 3 && t->document()->box() == nullptr);
}

// The roll and the timeline of a score box are views of one document: the box's own roll, the roll's window and the
// timeline's operations edit the same ScoreDoc; an edit in the roll is in the timeline's history and an edit in the
// timeline in the roll's, each undone from either; the selection is one
TEST(roll_and_timeline_share_one_document) {
    Rig r(kPatch);
    give(r, "s", three());
    Box* box = r.box("s");
    auto doc = ScoreDoc::ofBox(box);
    RollCore* inl = inlineRollOf(box);
    CHECK(inl && inl->compact);
    auto* inlineTarget = dynamic_cast<ScoreDocTarget*>(inl->target());
    CHECK(inlineTarget && inlineTarget->document() == doc);         // the box's own roll: the same document
    auto t = makeRollTarget(doc);                                     // the roll's window
    CHECK(t->document() == doc && ScoreDoc::ofBox(box) == doc);
    int told = 0;
    int token = doc->listen([&](unsigned what) { if (what & ScoreDoc::Content) ++told; });
    // an edit in the roll: the timeline's document has it, as one step named by the roll
    uint64_t g0 = t->generation();
    CHECK(t->moveEvents({}, {2}, 0.25, 0));
    CHECK(doc->score().find(2)->at == 0.75 && doc->canUndo() && doc->undoName() == "move" && told == 1);
    CHECK(t->generation() != g0 && t->score()->find(2)->at == 0.75);
    CHECK(scoreOfBox(box)->find(2)->at == 0.75);                       // and the box plays it
    // an edit in the timeline (its operations on the document): the roll sees it, and undoes it
    CHECK(tl::moveClips(*doc, {3}, 1.0));
    CHECK(t->score()->find(3)->at == 2 && inlineTarget->score()->find(3)->at == 2);
    CHECK(t->undo() && t->score()->find(3)->at == 1 && doc->score().find(3)->at == 1);
    // the roll's edit undone from the timeline's side (the document), redone from the box's own roll
    CHECK(doc->undo() && t->score()->find(2)->at == 0.5);
    CHECK(inlineTarget->redo() && doc->score().find(2)->at == 0.75);
    CHECK(doc->historySteps() == 2);
    // one selection: chosen in the roll, seen by the document (the timeline); chosen there, seen in the roll
    RollCore win(t);
    win.setSize(600, 300);
    win.selection = {1, 3};
    win.syncSelection();
    CHECK((doc->selection() == std::vector<uint64_t>{1, 3}));
    inl->syncSelection();
    CHECK((inl->selection == std::set<uint64_t>{1, 3}));
    doc->select({2});
    CHECK(win.syncSelection() && (win.selection == std::set<uint64_t>{2}));
    // the box's select message: the document's selection too
    box->message(0, {Atom("select"), Atom("row"), Atom("Fl")});
    CHECK(doc->selection().size() == 3);
    doc->unlisten(token);
}

// The cents of a note are always shown, in the box too (a number by the head, or a tick when the box is too small for
// one), and edited exactly: Alt+Up and Alt+Down a cent, with Shift ten, the List's cell (setField), each one undo step
TEST(roll_cents_shown_and_edited) {
    Rig r(kPatch);
    auto s = three();
    s->find(1)->cents = 19;                                          // orchidea's: the partial's detuning
    s->find(3)->pitch = 67.5;                                        // a quarter tone: +50
    give(r, "s", s);
    Box* box = r.box("s");
    RollCore* inl = inlineRollOf(box);
    inl->setSize(358, 140);                                          // the box's default size
    RollCore::CentsMark m = inl->centsMark(1);
    CHECK(m.kind == RollCore::CentsMark::Text && m.text == "+19");
    CHECK(inl->centsMark(3).kind == RollCore::CentsMark::Text && inl->centsMark(3).text == "+50");
    CHECK(inl->centsMark(2).kind == RollCore::CentsMark::None);
    inl->setSize(120, 12);                                           // no room for the number: a tick
    CHECK(inl->centsMark(1).kind == RollCore::CentsMark::Tick && inl->centsMark(1).text == "+19");
    // the keys on the selected note
    auto t = makeRollTarget(ScoreDoc::ofBox(box));
    RollCore c(t);
    c.setSize(678, 400);
    c.selection = {1};
    size_t steps = t->document()->historySteps();
    CHECK(c.key(FL_Up, FL_ALT) && now(r, "s")->find(1)->cents == 20);
    CHECK(c.key(FL_Up, FL_ALT | FL_SHIFT) && now(r, "s")->find(1)->cents == 30);
    CHECK(c.key(FL_Down, FL_ALT) && now(r, "s")->find(1)->cents == 29);
    CHECK(now(r, "s")->find(1)->pitch == 60 && now(r, "s")->find(1)->at == 0);     // nothing else moved
    CHECK(t->document()->historySteps() == steps + 3 && t->document()->undoName() == "detune");
    // the List's cell
    CHECK(t->setField({}, {2}, "cents", sc::Datum::number(-41)) && now(r, "s")->find(2)->cents == -41);
    CHECK(!t->setField({}, {2}, "cents", sc::Datum::symbol("sharp")) && now(r, "s")->find(2)->cents == -41);
    for (int k = 0; k < 4; ++k) CHECK(t->undo());
    CHECK(now(r, "s")->find(1)->cents == 19 && now(r, "s")->find(2)->cents == 0);
    // a sound or a message has no cents: nothing to detune
    c.selection = {};
    CHECK(!t->detune({}, {}, 1));
}
