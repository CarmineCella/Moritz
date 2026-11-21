// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// objects_score.cpp — transport, the patch's clock, and score, a score that plays itself on it.
//
// The score is Musil's C++ structure (musil/src/music/score.h), read from .score files with Musil's reader as data
// (score_text.h: nothing runs). The score object turns it into a plan off the audio thread: its sounds, by onset,
// for the audio thread (Published: never the interpreter, never an allocation there), its notes and messages, by
// onset, for the patch's scheduler. The audio thread renders a sound at the transport's position (a pure function
// of the position: a locate, a loop or a replaced score needs no state), so a sound starts on its sample; a note
// leaves its outlet at the logical time whose stamp is its sample, so a signal object it reaches starts there too.

#include "roll.h"
#include "score_doc.h"
#include "score_window.h"
#include "transport.h"
#include "zoom.h"

#include "music/score.h"
#include "music/score_text.h"

#include <FL/Fl.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cmath>
#include <deque>
#include <filesystem>
#include <map>
#include <set>
#include <mutex>

namespace moritz {

namespace sc = musil::score;

// ---------------------------------------------------------------- scores handed between threads and objects
// A code box's (out k score) sends `score <token>`: the token names a copy of the score made on the code's thread
// (without what only Musil holds), kept among the last 64 handed over; a score box takes it.
namespace {
std::mutex& handoffMutex() { static std::mutex m; return m; }
std::deque<std::pair<std::string, std::shared_ptr<const sc::Score>>>& handoff() {
    static std::deque<std::pair<std::string, std::shared_ptr<const sc::Score>>> d;
    return d;
}
}  // namespace

std::string shareScore(std::shared_ptr<const sc::Score> s) {
    std::lock_guard<std::mutex> g(handoffMutex());
    static unsigned long n = 0;
    std::string token = "score-" + std::to_string(++n);
    auto& d = handoff();
    d.emplace_back(token, std::move(s));
    while (d.size() > 64) d.pop_front();
    return token;
}

std::shared_ptr<const sc::Score> sharedScore(const std::string& token) {
    std::lock_guard<std::mutex> g(handoffMutex());
    for (auto& kv : handoff())
        if (kv.first == token) return kv.second;
    return nullptr;
}

namespace {

std::string fmtTime(double sec) {
    char b[32];
    int m = (int)(sec / 60);
    std::snprintf(b, sizeof b, "%d'%02d\"", m, (int)std::floor(sec - 60 * m));
    return b;
}
Atom atomOf(const sc::Datum& d) {
    if (d.kind == sc::Datum::Num) return Atom((float)d.num);
    return Atom(d.s);
}
void appendDatum(Message& m, const sc::Datum& d) {
    if (d.kind == sc::Datum::List) { for (auto& x : d.list) appendDatum(m, x); return; }
    m.push_back(atomOf(d));
}
bool endsWith(const std::string& s, const char* e) {
    size_t n = std::strlen(e);
    return s.size() >= n && s.compare(s.size() - n, n, e) == 0;
}

// ---------------------------------------------------------------- transport
// transport [bpm] [num den | num/den]: the patch's clock (one for the whole patch, its subpatches and help patches;
// several transport objects show and drive the same one). Times in the scores are seconds; bars and beats are
// counted at the tempo (beats a minute; a beat is 1/den) and meter.
//   play, 1 / stop, 0        start (from where it is) and stop
//   locate bar [beat [tick]] go to a bar (from 1; 480 ticks a beat); seek ms: to a time
//   loop bar1 bar2           play from bar1 up to bar2 again and again; loop 0 (or off): no loop
//   tempo bpm, meter n d     the tempo and meter bars and beats are counted in
//   bang                     the position now
// Outlets: the bar, the beat, the position in ms (at every beat while it plays, and at every change), and 1 or 0
// when it starts or stops.
class TransportBox : public Box {
public:
    explicit TransportBox(const Message& a) {
        setPorts({P::Control}, {P::Control, P::Control, P::Control, P::Control});
        std::vector<float> nums;
        for (auto& x : a) {
            if (x.isFloat()) nums.push_back(x.f);
            else if (auto k = x.s.find('/'); k != std::string::npos) {
                nums.resize(std::max<size_t>(nums.size(), 1), 120.f);
                nums.push_back((float)std::atof(x.s.substr(0, k).c_str()));
                nums.push_back((float)std::atof(x.s.substr(k + 1).c_str()));
            }
        }
        if (!nums.empty() && nums[0] > 0) bpm_ = nums[0];
        if (nums.size() >= 3 && nums[1] >= 1 && nums[2] >= 1) { num_ = (int)nums[1]; den_ = (int)nums[2]; }
    }
    ~TransportBox() override { if (t_) t_->unlisten(this); }
    void attached() override {
        t_ = transportOf(patch);
        if (bpm_ > 0) t_->setTempo(patch, bpm_);
        if (num_ > 0) t_->setMeter(patch, num_, den_);
        t_->listen(this, [this] { changed(); });
    }
    void audioChanged() override { if (t_ && t_->playing()) t_->reanchor(patch); }
    void message(int, const Message& m) override {
        if (!t_ || m.empty()) return;
        const Atom& h = m[0];
        if (h.is("play") || h.is("start") || (h.isFloat() && h.f != 0.f)) t_->play(patch);
        else if (h.is("stop") || h.isFloat()) t_->stop(patch);
        else if (h.is("bang")) report();
        else if (h.is("locate")) t_->locate(patch, t_->secondsOf(argf(m, 1, 1.f), argf(m, 2, 1.f), argf(m, 3, 0.f)));
        else if (h.is("seek")) t_->locate(patch, argf(m, 1, 0.f) / 1000.0);
        else if (h.is("rewind")) t_->locate(patch, 0);
        else if (h.is("loop")) {
            if (m.size() >= 3 && m[1].isFloat() && m[2].isFloat() && m[2].f > m[1].f)
                t_->setLoop(patch, true, t_->secondsOf(m[1].f, 1, 0), t_->secondsOf(m[2].f, 1, 0));
            else t_->setLoop(patch, false);
        }
        else if (h.is("tempo") && m.size() > 1 && m[1].isFloat()) t_->setTempo(patch, m[1].f);
        else if (h.is("meter") && m.size() > 2 && m[1].isFloat() && m[2].isFloat()) t_->setMeter(patch, (int)m[1].f, (int)m[2].f);
        else patch->print("transport: unknown message " + h.str());
    }

private:
    std::shared_ptr<Transport> t_;
    float bpm_ = 0;
    int num_ = 0, den_ = 4;
    bool wasPlaying_ = false;

    void report() {
        int lap; double pos;
        t_->where(patch, lap, pos);
        long bar; int beat, tick;
        t_->bbt(pos, bar, beat, tick);
        outletFloat(2, (float)(pos / t_->sampleRate() * 1000.0));
        outletFloat(1, (float)beat);
        outletFloat(0, (float)bar);
    }
    void changed() {
        bool here = false;                       // a removed box is kept until the audio thread lets go: it is quiet then
        for (auto& b : patch->boxes) here = here || b.get() == this;
        if (!here) return;
        patch->cancel(this);
        if (t_->playing() != wasPlaying_) { wasPlaying_ = t_->playing(); outletFloat(3, wasPlaying_ ? 1.f : 0.f); }
        report();
        if (t_->playing()) next();
    }
    // the next beat (or the loop's start, when the loop comes first), on its sample
    void next() {
        int lap; double pos;
        t_->where(patch, lap, pos);
        double beat = t_->beatSamples();
        double b = (std::floor(pos / beat + 1e-9) + 1) * beat;
        double t = t_->timeAt(std::floor(b + 1e-6), lap);
        if (std::isnan(t) && t_->looping()) t = t_->timeAt((double)t_->loopStart(), lap + 1);
        if (std::isnan(t)) return;
        patch->scheduleAt(this, t, [this] { report(); next(); });
    }
};

// ---------------------------------------------------------------- score
// What the audio thread plays: the sounds of the score by onset, their sources, where they go.
struct ScorePlan {
    struct Voice {
        int64_t start = 0, len = 0;      // positions, samples
        double from = 0, to = -1;        // seconds in the source
        float gain = 1.f;
        double speed = 1;                // a note's sound: faster or slower by its pitch against the sample's
        int file = -1, buf = -1;         // a decoded file, or a buffer~ by name
        int gains = 0;                   // where its channel gains start in `gains`
        int64_t fadeIn = 0, fadeOut = 0; // its fades (the clip's fade-in and fade-out), samples
    };
    std::vector<Voice> voices;
    std::vector<float> gains;            // per voice: one per outlet (a mono source's placement)
    int64_t maxLen = 0;
    double sr = 44100.0;
    std::vector<std::shared_ptr<const SampleBuffer>> files;
    std::vector<std::unique_ptr<BufferRef>> bufs;
};

// score name [file.score] [channels] [@size w h] [@view timeline|roll|piano|list]: a score (rows, notes, sounds,
// messages, scores inside it) that plays on the patch's transport. Without a file it is kept in the patch. The box
// draws its score as a roll (roll.h), edited in place when the patch is locked (select, drag to move, drag an end to
// resize, Alt-drag copies, Delete, the arrows nudge, Alt+Up and Alt+Down detune by a cent); a double-click (or its
// menu's Open) opens the score's window (score_window.h) in the view used last for it, the first time in its @view
// (Roll when none); in edit mode its corner sizes it. Outlets: one signal for each
// channel (2 by default: its sounds, and the notes that carry a sound of their own, placed at their azimuths), then
// notes (pitch velocity duration-ms row dynamics technique; - for none), then messages (name values...; a patch event
// as patch name duration-ms args...), then info (end when it plays past its last event; the summary for info). A
// message is also sent to the receive objects of its name (automation lanes drive a patch's receives). The rows are
// tracks: their mixer strips (gain, az, mute, solo: score.h's Mixer) apply to the sounds, notes and patches on them; a
// sound's fade-in and fade-out are played.
//   read [file], write [file]   a .score file (Musil data, read without running anything); write: in the patch's
//                               folder or ~/Documents/Moritz
//   clear                       no events
//   add [note] at dur row pitch [dyn [tech]] | add [note] at row pitch [dyn [tech]] (a beat long)
//   add sound at dur file [from [to]] | add msg at name values...
//   select all | none | id... | row name | time t0 t1      what the transformations below change (none: all of it)
//   transpose n, invert [pitch], retrograde, augment k, diminish k, shift seconds, quantize [grid-seconds [strength]],
//   humanize [ms [dB [seed]]], thin keep [seed], density v... [seed]   (music/score_transform.h, as Musil's)
//   play, stop, locate bar [beat], seek ms   the transport's (the same for every score of the patch)
//   info                        rows events seconds, out of the info outlet
//   score <token>               a score a code box (or orchidea) sent ((out k s) with s a Musil score)
//   open [view]                 the score's window (timeline, roll, piano, list; none: the view used last)
// The box's roll and its window's views are views of one document (score_doc.h): an edit in the box is
// an undo step of the score's history (Edit > Undo with the box clicked last takes it back, the timeline's and the
// roll's Undo too), and marks the patch edited; the messages above change the score from outside (the document
// follows them, its history starting again), as the code view and the patch's undo do.
class ScoreBox : public Box {
public:
    explicit ScoreBox(const Message& a) {
        kind = BoxKind::UI;
        for (size_t k = 0; k < a.size(); ++k) {
            const Atom& x = a[k];
            if (x.is("@size")) {
                w_ = std::clamp((int)argf(a, k + 1, (float)kDefaultW), 120, 4000);
                h_ = std::clamp((int)argf(a, k + 2, (float)kDefaultH), 50, 3000);
                k += 2;
            }
            else if (x.is("@view")) {                    // the view its window opens in the first time
                ScoreView v;
                if (k + 1 < a.size() && parseScoreView(a[k + 1].str(), v)) view_ = (int)v;
                ++k;
            }
            else if (x.isFloat()) nOut_ = std::clamp((int)x.f, 1, 16);
            else if (endsWith(x.s, ".score")) file_ = x.s;
            else if (name_.empty()) name_ = x.s;
        }
        std::vector<Port> outs((size_t)nOut_, P::Signal);
        outs.insert(outs.end(), {P::Control, P::Control, P::Control});
        setPorts({P::Control}, outs);
        score_ = std::make_shared<sc::Score>(name_.empty() ? std::string("score") : name_, 44100);
    }
    ~ScoreBox() override {
        if (t_) t_->unlisten(this);
        inline_.reset();                         // its roll's target lets go of the document (the windows keep theirs)
    }

    Message saveArgs() const override {
        Message m;
        if (!name_.empty()) m.push_back(Atom(name_));
        if (!file_.empty()) m.push_back(Atom(file_));
        if (nOut_ != 2) m.push_back(Atom((float)nOut_));
        if (w_ != kDefaultW || h_ != kDefaultH) { m.push_back(Atom("@size")); m.push_back(Atom((float)w_)); m.push_back(Atom((float)h_)); }
        if (view_ >= 0) {
            std::string v = scoreViewName((ScoreView)view_);
            m.push_back(Atom("@view"));
            m.push_back(Atom(v == "piano roll" ? std::string("piano") : v));
        }
        return m;
    }
    // kept in the patch when it has no file of its own
    std::string script() const override { return file_.empty() && !score_->empty() ? sc::toText(*score_) : ""; }
    void setScript(const std::string& text) override {
        try {
            auto s = sc::readText(text, here());
            if (!name_.empty()) s->name = name_;
            score_ = s;
        } catch (const std::exception& e) { if (patch) patch->print(std::string("score: ") + e.what()); }
    }
    void attached() override {
        t_ = transportOf(patch);
        t_->listen(this, [this] { follow(); });
        if (!file_.empty()) readFile(file_);
        else rebuild();
    }
    void resolve() override {
        // the buffer~s named by the plan's sounds may have come or gone: looked up again (atomics)
        if (ScorePlan* p = plan_.get()) for (auto& b : p->bufs) b->resolve(patch);
        if (plan_.get() && plan_.get()->sr != patch->root()->sampleRate.load()) rebuild();
    }
    void audioChanged() override { if (t_ && t_->playing()) t_->reanchor(patch); }
    void message(int, const Message& m) override {
        if (m.empty()) return;
        const Atom& h = m[0];
        if (h.is("read")) readFile(m.size() > 1 ? m[1].str() : file_);
        else if (h.is("write")) writeFile(m.size() > 1 ? m[1].str() : file_);
        else if (h.is("clear")) { auto s = score_->clone(); s->clear(); replace(s); }
        else if (h.is("add")) add(m);
        else if (h.is("select")) select(m);
        else if (h.is("transpose") || h.is("invert") || h.is("retrograde") || h.is("augment") || h.is("diminish") ||
                 h.is("shift") || h.is("quantize") || h.is("humanize") || h.is("thin") || h.is("density")) transformBy(m);
        else if (h.is("open")) {
            ScoreView v;
            bool given = m.size() > 1 && parseScoreView(m[1].str(), v);
            openScoreBox(this, given ? (int)v : -1);
        }
        else if (h.is("play") || h.is("start")) { if (t_) t_->play(patch); }
        else if (h.is("stop")) { if (t_) t_->stop(patch); }
        else if (h.is("locate") && t_) t_->locate(patch, t_->secondsOf(argf(m, 1, 1.f), argf(m, 2, 1.f), argf(m, 3, 0.f)));
        else if (h.is("seek") && t_) t_->locate(patch, argf(m, 1, 0.f) / 1000.0);
        else if (h.is("info")) outlet(nOut_ + 2, {Atom("info"), Atom((float)rows_), Atom((float)score_->size()), Atom((float)score_->duration())});
        else if (h.is("score") && m.size() > 1) {
            auto s = sharedScore(m[1].str());
            if (!s) { patch->print("score: no score " + m[1].str() + " (a code box sends one with (out k s))"); return; }
            replace(s->clone());
        }
        else patch->print("score: unknown message " + h.str());
    }
    void dsp(int n) override;

    // ---- the interface: a header (name and summary) and the score as a roll, edited in place when locked
    void uiSize(int& w, int& h) const override {      // measured without the fonts: made where there is no display too
        w = w_;
        h = h_;
    }
    bool uiResizable() const override { return true; }
    void uiResize(int w, int h) override {
        w_ = std::clamp(w, 120, 4000);
        h_ = std::clamp(h, 50, 3000);
    }
    void drawUI(int X, int Y) override {
        zd::font(kFont, kFontSize);
        fl_color(kText);
        int head = kBoxH;
        std::string title = "score" + (name_.empty() ? std::string() : " " + name_) + (file_.empty() ? std::string() : " " + file_);
        zd::draw(title.c_str(), X + 8, Y + head / 2 + zd::height() / 2 - zd::descent());
        zd::font(kFont, std::max(8, kFontSize - 3));
        fl_color(kDim);
        std::string s = std::to_string(rows_) + " rows · " + std::to_string(score_->size()) + " events · " + fmtTime(score_->duration());
        int sw = (int)zd::width(s.c_str());
        zd::font(kFont, kFontSize);
        int tw = (int)zd::width(title.c_str());
        zd::font(kFont, std::max(8, kFontSize - 3));
        zd::draw(s.c_str(), X + std::max(8 + tw + 12, w_ - sw - 8), Y + head / 2 + 4);
        RollCore& c = roll();
        c.setSize(w_ - 2, h_ - head - 6);
        if (t_) {
            int lap;
            double pos;
            t_->where(patch, lap, pos);
            c.playhead = t_->playing() || pos > 0 ? t_->seconds(pos) : NAN;
        }
        c.draw(X + 1, Y + head);
    }
    // locked: the roll edited in place; a double-click opens the score's window (on a score placed inside: the roll,
    // into it)
    void uiMouse(int event, int mx, int my) override {
        RollCore& c = roll();
        bool dbl = event == FL_PUSH && Fl::event_clicks();
        if (dbl) Fl::event_clicks(0);
        if (event == FL_PUSH && my < kBoxH) {              // the header
            if (dbl) openScoreBox(this);
            return;
        }
        if (!c.mouse(event, mx - 1, my - kBoxH, Fl::event_state(), dbl ? 1 : 0) && dbl) openScoreBox(this);
    }
    bool uiKey(int key) override { return roll().key(key, Fl::event_state()); }
    bool uiUndo(bool redo) override {                  // the score's own history (its edits in the box, the roll, the timeline)
        RollEditTarget* t = roll().target();
        if (!t || !(redo ? t->canRedo() : t->canUndo())) return false;
        if (redo) t->redo();
        else t->undo();
        if (patch) patch->redraw();
        return true;
    }
    // (the canvas's menu has Open for a score box: its window, in the view used last)
    std::vector<std::string> menuItems() override { return {"Read...", "Write..."}; }
    void menuAction(int k) override {
        std::string f = chooseFile(k == 1, k == 0 ? "Read a score" : "Write the score", "Score\t*.score", file_.empty() ? name_ + ".score" : file_);
        if (!f.empty()) message(0, {Atom(k == 0 ? "read" : "write"), Atom(f)});
    }

    // the score from elsewhere (GUI thread): what plays changes at once, the transport going on
    void replace(std::shared_ptr<sc::Score> s) {
        if (!name_.empty() && s->name != name_) s->name = name_;
        score_ = std::move(s);
        edited();
    }
    // what an editor binds to (score_doc.h): the score now, and whether it is kept in the patch (no file of its own)
    std::shared_ptr<sc::Score> current() const { return score_; }
    bool keptInPatch() const { return file_.empty(); }
    // the view its window opens in: the one used last, else its @view, else Roll
    int openView() const { return lastView_ >= 0 ? lastView_ : view_ >= 0 ? view_ : (int)ScoreView::Roll; }
    void setLastView(int v) { lastView_ = v; }
    // the roll inside the box (made when first drawn, clicked or asked for its selection), on the score's document
    RollCore& roll() {
        if (!inline_) {
            inline_ = std::make_unique<RollCore>(nullptr);
            inline_->compact = true;
            inline_->onChange = [this] { if (patch) patch->redraw(); };
            inline_->onEnter = [this](uint64_t sub) {
                ScoreWindow* w = openScoreBox(this, (int)ScoreView::Roll);
                if (w && sub) { w->roll()->core().enter(sub); w->roll()->changed(); }
            };
        }
        if (!inline_->target() && patch)
            if (auto t = makeRollTarget(ScoreDoc::ofBox(this))) {
                t->setListener([this](unsigned) { if (patch) patch->redraw(); });   // the timeline's edits, its selection
                inline_->setTarget(t);
            }
        return *inline_;
    }

private:
    struct Ctl {
        int64_t pos;
        int outlet;
        Message msg;
        bool send = false;                       // a message: to the receives of its name too
    };
    static constexpr int kDefaultW = 360, kDefaultH = 150;
    int nOut_ = 2;
    int w_ = kDefaultW, h_ = kDefaultH;
    int view_ = -1, lastView_ = -1;              // @view (saved), the view used last (not saved)
    std::string name_, file_;
    std::shared_ptr<sc::Score> score_;           // never changed in place: a new one for every change (the views keep theirs)
    std::unique_ptr<RollCore> inline_;
    std::shared_ptr<Transport> t_;
    Published<ScorePlan> plan_;
    std::vector<Ctl> ctl_;                       // notes and messages by onset (GUI)
    int64_t end_ = 0;                            // where the last event ends
    std::map<std::string, std::shared_ptr<const SampleBuffer>> decoded_;   // files read once (GUI)
    // what the scheduler is on: the pass through the loop and the next control event
    int lap_ = 0;
    size_t cursor_ = 0;
    bool ended_ = false;
    bool firing_ = false;
    int64_t firedPos_ = -1;
    int firedLap_ = 0;
    size_t rows_ = 0;                            // the rows declared or used

    // where the score kept in the patch is, for its errors and the files it places: the patch's folder
    std::string here() const {
        std::string dir = patch ? dirOf(patch->resolvePath("x")) : std::string();
        return (dir.empty() ? std::string(".") : dir) + "/score " + id;
    }
    std::string folderOf(const sc::Score* s) const { return s && !s->path.empty() ? sc::folderOf(s->path) : std::string(); }
    std::string sourcePath(const std::string& src, const sc::Score* origin) const {
        if (src.empty() || isAbsolutePath(src)) return src;
        std::string dir = folderOf(origin);
        if (dir.empty()) dir = folderOf(score_.get());
        if (!dir.empty()) {
            std::string f = dir + "/" + src;
            std::error_code ec;
            if (std::filesystem::exists(f, ec)) return f;
        }
        return patch->resolvePath(src);
    }

    void readFile(const std::string& f) {
        if (f.empty()) { patch->print("score: read what? (read file.score)"); return; }
        try {
            auto s = sc::readFile(patch->resolvePath(f));
            if (!name_.empty()) s->name = name_;
            score_ = s;
            edited();
        } catch (const std::exception& e) { patch->print(std::string("score: ") + e.what()); }
    }
    bool writeFile(const std::string& f, std::string* errOut = nullptr) {
        auto fail = [&](const std::string& e) { if (errOut) *errOut = e; else patch->print("score: " + e); return false; };
        if (f.empty()) return fail("write where? (write file.score)");
        std::string path = patch->resolvePath(f);
        // a patch from elsewhere does not write anywhere: its folder, or ~/Documents/Moritz
        std::vector<std::string> roots;
        const Patch* q = patch;
        while (q && q->dir.empty() && q->parent) q = q->parent;
        if (q && !q->dir.empty()) roots.push_back(q->dir);
        if (std::string home = homeDir(); !home.empty()) roots.push_back(home + "/Documents/Moritz");
        if (!musil::path_under(musil::resolved_path(path), roots))
            return fail("not written: " + path + " is outside the patch's folder and ~/Documents/Moritz");
        try {
            sc::writeFile(*score_, path);            // (its document, if one is open, keeps its history)
            score_->path = path;
        } catch (const std::exception& e) { return fail(e.what()); }
        return true;
    }
    // add [note] at dur row pitch [dyn [tech]] | add [note] at row pitch ... : the words read as the score file's (note
    // ...) form (a note without its duration is a beat long)
    void add(const Message& m0) {
        Message m = m0;
        if (m.size() > 1 && m[1].isFloat()) m.insert(m.begin() + 1, Atom("note"));            // the kind left out: a note
        if (m.size() > 4 && m[1].is("note") && m[2].isFloat() && !m[3].isFloat())              // no duration: a beat
            m.insert(m.begin() + 3, Atom((float)(60.0 / std::max(1.0, score_->bpm))));
        if (m.size() < 3 || !m[1].isSymbol()) { patch->print("score: add note|sound|msg at ..."); return; }
        std::string form = "(score \"x\" (";
        for (size_t k = 1; k < m.size(); ++k) {
            if (k > 1) form += ' ';
            if (k == 3 && m[1].is("sound")) form += sc::quoted(m[k].str());
            else form += m[k].isFloat() ? sc::numText(m[k].f) : sc::wordText(m[k].s);
        }
        form += "))";
        try {
            auto one = sc::readText(form, here());
            auto s = score_->clone();
            for (auto& e : one->events()) {
                auto c = std::make_shared<sc::Event>(*e);
                c->id = 0;
                s->insert(c);
            }
            replace(s);
        } catch (const std::exception& e) { patch->print(std::string("score: ") + e.what()); }
    }
    // select all | none | id... | row name | time t0 t1
    void select(const Message& m) {
        RollCore& c = roll();
        c.syncSelection();                       // the document's selection, then this one in its place
        c.path.clear();
        c.selection.clear();
        std::string how = m.size() > 1 && m[1].isSymbol() ? m[1].s : "";
        if (how == "all") c.selectAll();
        else if (how == "row" && m.size() > 2) { for (auto& e : score_->events()) if (e->row == m[2].str()) c.selection.insert(e->id); }
        else if (how == "time" && m.size() > 2) {
            double t0 = argf(m, 2, 0.f), t1 = argf(m, 3, 1e9f);
            score_->query(t0, t1, [&](const sc::Event& e) { c.selection.insert(e.id); });
        } else if (how.empty()) {
            for (size_t k = 1; k < m.size(); ++k) if (m[k].isFloat() && m[k].f >= 1 && score_->find((uint64_t)m[k].f)) c.selection.insert((uint64_t)m[k].f);
        } else if (how != "none") { patch->print("score: select all, none, ids, row name or time t0 t1"); return; }
        c.syncSelection();                       // the timeline and the roll's window show it too
        if (patch) patch->redraw();
    }
    // a transformation of the selection (all of it when nothing is selected), as Musil's (music/score_transform.h)
    void transformBy(const Message& m) {
        ScoreTransform t;
        const Atom& h = m[0];
        auto num = [&](size_t k, double def) { float v = argf(m, k, (float)def); return std::isfinite(v) ? (double)v : def; };
        t.seed = (uint32_t)num(h.is("humanize") ? 3 : 2, 1);
        if (h.is("transpose")) { t.kind = ScoreTransform::Transpose; t.a = num(1, 0); }
        else if (h.is("invert")) {
            t.kind = ScoreTransform::Invert;
            t.a = m.size() > 1 ? (m[1].isFloat() ? m[1].f : sc::pitchOfName(m[1].str())) : NAN;
            if (m.size() > 1 && t.a < 0) { patch->print("score: invert around what? (a pitch: 60 or C4)"); return; }
        }
        else if (h.is("retrograde")) t.kind = ScoreTransform::Retrograde;
        else if (h.is("augment") || h.is("diminish")) {
            t.kind = ScoreTransform::Augment;
            double k = num(1, 2);
            if (!(k > 0)) { patch->print("score: " + h.s + " by a factor > 0"); return; }
            t.a = h.is("augment") ? k : 1 / k;
        }
        else if (h.is("shift")) { t.kind = ScoreTransform::Shift; t.a = num(1, 0); }
        else if (h.is("quantize")) { t.kind = ScoreTransform::Quantize; t.a = num(1, roll().snapSeconds() > 0 ? roll().snapSeconds() : 0.25); t.b = num(2, 1); }
        else if (h.is("humanize")) { t.kind = ScoreTransform::Humanize; t.a = num(1, 20) / 1000; t.b = num(2, 1.5); }
        else if (h.is("thin")) { t.kind = ScoreTransform::Thin; t.a = num(1, 0.5); }
        else if (h.is("density")) {
            t.kind = ScoreTransform::Density;
            for (size_t k = 1; k < m.size(); ++k) if (m[k].isFloat()) t.curve.push_back(std::clamp((double)m[k].f, 0.0, 1.0));
            t.seed = 1;
        }
        RollCore& c = roll();
        c.syncSelection();                       // what is selected in any view of the score
        auto s = score_->clone();
        try {
            if (!applyTransform(*s, c.path.empty() ? c.selectedIds() : Ids{}, t)) return;
        } catch (const std::exception& e) { patch->print(std::string("score: ") + e.what()); return; }
        for (auto it = c.selection.begin(); it != c.selection.end();) it = s->find(*it) ? std::next(it) : c.selection.erase(it);
        replace(s);
    }
    void edited() {
        rebuild();
        if (patch) patch->redraw();
    }

    // a voice playing a region of a sound file (decoded once, kept while an event reads it)
    void addVoice(ScorePlan& plan, std::map<std::string, int>& files, int64_t start, int64_t len, double from, double to, float gain,
                  double speed, double az, int64_t fadeIn, int64_t fadeOut, const std::string& path) {
        if (len <= 0) return;
        auto it = files.find(path);
        if (it == files.end()) {
            auto d = decoded_.find(path);
            if (d == decoded_.end()) {
                auto b = std::make_shared<SampleBuffer>();
                std::string err;
                if (!decodeAudioFile(path, *b, err)) { patch->print("score: " + err); b.reset(); }
                d = decoded_.emplace(path, b).first;
            }
            if (!d->second) return;
            it = files.emplace(path, (int)plan.files.size()).first;
            plan.files.push_back(d->second);
        }
        ScorePlan::Voice v;
        v.start = start;
        v.len = len;
        v.from = from;
        v.to = to;
        v.gain = gain;
        v.speed = std::isfinite(speed) && speed > 0 ? speed : 1;
        v.fadeIn = fadeIn;
        v.fadeOut = fadeOut;
        v.file = it->second;
        v.gains = (int)plan.gains.size();
        double g[16];
        sc::panGains(az, nOut_, g);
        for (int c = 0; c < nOut_; ++c) plan.gains.push_back((float)g[c]);
        plan.maxLen = std::max(plan.maxLen, v.len);
        plan.voices.push_back(v);
    }

    // ---- the plan: made on the GUI thread, published; the control events kept here
    void rebuild() {
        if (!patch) return;
        double sr = patch->root()->sampleRate.load();
        auto plan = std::make_unique<ScorePlan>();
        plan->sr = sr;
        std::vector<sc::Placed> flat;
        try { sc::flatten(*score_, flat); }
        catch (const std::exception& e) { patch->print(std::string("score: ") + e.what()); flat.clear(); }
        const sc::Mixer mixer(*score_);           // the tracks' strips: each event plays on the row that placed it
        std::vector<Ctl> ctl;
        std::map<std::string, int> files, bufs;
        end_ = 0;
        auto at = [sr](double sec) { return (int64_t)std::floor(sec * sr); };   // as Musil's score-render places them
        for (auto& p : flat) {
            const sc::Event& e = p.ev;
            end_ = std::max(end_, at(e.end()));
            const std::string& track = p.top ? p.top->row : e.row;
            if (e.kind != sc::Kind::Message && !mixer.sounds(track)) continue;   // muted, or another track soloed
            const sc::Strip strip = mixer.of(track);
            if (e.kind == sc::Kind::Sound) {
                ScorePlan::Voice v;
                v.start = at(e.at);
                v.len = at(e.dur);
                if (v.len <= 0) continue;
                v.from = e.from;
                v.to = e.to;
                v.gain = (float)(e.gain * strip.gain);
                v.fadeIn = std::clamp<int64_t>(at(sc::numField(e.extra, "fade-in", 0)), 0, v.len);
                v.fadeOut = std::clamp<int64_t>(at(sc::numField(e.extra, "fade-out", 0)), 0, v.len);
                if (e.buffer) {
                    auto it = bufs.find(e.source);
                    if (it == bufs.end()) {
                        it = bufs.emplace(e.source, (int)plan->bufs.size()).first;
                        plan->bufs.push_back(std::make_unique<BufferRef>(e.source));
                        plan->bufs.back()->resolve(patch);
                    }
                    v.buf = it->second;
                } else {
                    addVoice(*plan, files, v.start, v.len, v.from, v.to, v.gain, 1.0, e.az + strip.az, v.fadeIn, v.fadeOut,
                             sourcePath(e.source, p.origin));
                    continue;
                }
                v.gains = (int)plan->gains.size();
                double g[16];
                sc::panGains(e.az + strip.az, nOut_, g);
                for (int c = 0; c < nOut_; ++c) plan->gains.push_back((float)g[c]);
                plan->maxLen = std::max(plan->maxLen, v.len);
                plan->voices.push_back(v);
            } else if (e.kind == sc::Kind::Note) {
                if (e.variant == "chord") continue;
                // a note with a sound of its own (orchidea's: its sample) plays it, faster or slower by its pitch against
                // the sample's (sound-pitch; without it, by its cents)
                if (const sc::Datum* snd = sc::field(e.extra, "sound"); snd && (snd->kind == sc::Datum::Str || snd->kind == sc::Datum::Sym) && !snd->s.empty()) {
                    const sc::Datum* sp = sc::field(e.extra, "sound-pitch");
                    double base = sp && sp->kind == sc::Datum::Num ? sp->num : e.pitch;
                    addVoice(*plan, files, at(e.at), at(e.dur), 0, -1, (float)(e.gain * strip.gain), std::pow(2.0, (e.sounding() - base) / 12.0),
                             e.az + strip.az, 0, 0,
                             sourcePath(snd->s, p.origin));
                }
                int vel = (int)std::lround(sc::velocityOf(e.dyn) * e.gain * strip.gain);
                Message msg{Atom((float)e.sounding()), Atom((float)std::clamp(vel, 1, 127)), Atom((float)(e.dur * 1000.0)),
                            Atom(e.row.empty() ? std::string("-") : e.row), Atom(e.dyn.empty() ? std::string("-") : e.dyn),
                            Atom(e.tech.empty() ? std::string("-") : e.tech)};
                ctl.push_back({at(e.at), nOut_, std::move(msg)});
            } else if (e.kind == sc::Kind::Message) {
                Message msg{Atom(e.source.empty() ? std::string("-") : e.source)};
                for (auto& d : e.values) appendDatum(msg, d);
                ctl.push_back({at(e.at), nOut_ + 1, std::move(msg), !e.source.empty()});
            } else if (e.kind == sc::Kind::Patch) {
                // patch clips do not run yet: they leave the messages outlet as `patch name duration-ms args...`
                // (the hook where a patch clip will be loaded and run while it lasts)
                Message msg{Atom("patch"), Atom(e.source), Atom((float)(e.dur * 1000.0))};
                for (auto& d : e.values) appendDatum(msg, d);
                ctl.push_back({at(e.at), nOut_ + 1, std::move(msg)});
            }
        }
        std::stable_sort(plan->voices.begin(), plan->voices.end(), [](const ScorePlan::Voice& a, const ScorePlan::Voice& b) { return a.start < b.start; });
        std::stable_sort(ctl.begin(), ctl.end(), [](const Ctl& a, const Ctl& b) { return a.pos < b.pos; });
        ctl_ = std::move(ctl);
        std::set<std::string> rows;
        for (auto& r : score_->rows) rows.insert(r.name);
        for (auto& e : score_->events()) if (!e->row.empty()) rows.insert(e->row);
        rows_ = rows.size();
        // files no event reads any more are let go (a plan the audio thread still uses keeps its own)
        for (auto it = decoded_.begin(); it != decoded_.end();) it = files.count(it->first) ? std::next(it) : decoded_.erase(it);
        plan_.set(patch, std::move(plan));
        follow();
    }

    // ---- the control events on the scheduler, following the transport
    // in its patch still (a removed box is kept until the audio thread lets go of it: it follows nothing then)
    bool inPatch() const {
        for (auto& b : patch->boxes) if (b.get() == this) return true;
        return false;
    }
    void follow() {
        if (!inPatch()) return;
        patch->cancel(this);
        if (!t_ || !t_->playing()) return;
        double pos;
        t_->where(patch, lap_, pos);
        ended_ = !t_->looping() && pos >= (double)end_ && end_ > 0;
        int64_t from = (int64_t)std::ceil(pos - 1e-6);
        if (firing_ && lap_ == firedLap_) from = std::max(from, firedPos_ + 1);   // what was just sent is not sent again
        cursor_ = (size_t)(std::lower_bound(ctl_.begin(), ctl_.end(), from, [](const Ctl& c, int64_t p) { return c.pos < p; }) - ctl_.begin());
        schedule();
    }
    void schedule() {
        bool looping = t_->looping();
        for (int guard = 0; guard < 2; ++guard) {
            if (cursor_ < ctl_.size() && (!looping || ctl_[cursor_].pos < t_->loopEnd())) {
                double t = t_->timeAt((double)ctl_[cursor_].pos, lap_);
                if (!std::isnan(t)) { patch->scheduleAt(this, t, [this] { fire(); }); return; }
            }
            if (!looping) break;
            // the rest of this pass has nothing: the next pass, from the loop's start
            ++lap_;
            cursor_ = (size_t)(std::lower_bound(ctl_.begin(), ctl_.end(), t_->loopStart(), [](const Ctl& c, int64_t p) { return c.pos < p; }) - ctl_.begin());
            if (cursor_ >= ctl_.size() || ctl_[cursor_].pos >= t_->loopEnd()) return;   // nothing in the loop
        }
        if (!looping && !ended_ && end_ > 0) {             // after the last event: its end, once
            double t = t_->timeAt((double)end_, 0);
            if (!std::isnan(t)) patch->scheduleAt(this, t, [this] { ended_ = true; outlet(nOut_ + 2, {Atom("end")}); });
        }
    }
    void fire() {
        if (cursor_ >= ctl_.size()) return;
        int64_t p = ctl_[cursor_].pos;
        std::vector<Ctl> due;                   // copied: an outlet may replace the score (and the plan)
        while (cursor_ < ctl_.size() && ctl_[cursor_].pos == p) due.push_back(ctl_[cursor_++]);
        uint64_t version = score_->version();
        auto keep = score_;
        firing_ = true;
        firedPos_ = p;
        firedLap_ = lap_;
        for (auto& c : due) {
            if (c.send) patch->sendTo(c.msg[0].str(), Message(c.msg.begin() + 1, c.msg.end()));   // the lanes' receives
            outlet(c.outlet, c.msg);
            if (score_ != keep || score_->version() != version || !t_->playing()) { firing_ = false; return; }   // rebuild() or the transport took over
        }
        firing_ = false;
        schedule();
    }
};

void ScoreBox::dsp(int n) {
    float* out[16];
    for (int c = 0; c < nOut_; ++c) {
        out[c] = sigOut(c);
        if (out[c]) dsp::fill(out[c], 0.f, n);
    }
    const ScorePlan* plan = plan_.get();
    const TransportState* ts = t_ ? t_->state() : nullptr;
    if (!plan || !ts || plan->voices.empty()) return;
    const double sr = plan->sr;
    const uint64_t t0 = patch->sampleTime();
    int i = 0;
    while (i < n) {
        int64_t pos;
        uint64_t t = t0 + (uint64_t)i;
        if (!ts->at(t, pos)) {                                  // not playing here: until the next change
            if (t < ts->since) { i += (int)std::min<int64_t>(n - i, (int64_t)(ts->since - t)); continue; }
            break;
        }
        int len = (int)ts->run(t, pos, n - i);
        int64_t p1 = pos + len;
        // the voices sounding in [pos, p1): starting before p1, ending after pos
        auto lo = std::lower_bound(plan->voices.begin(), plan->voices.end(), pos - plan->maxLen,
                                   [](const ScorePlan::Voice& v, int64_t q) { return v.start < q; });
        for (auto it = lo; it != plan->voices.end() && it->start < p1; ++it) {
            const ScorePlan::Voice& v = *it;
            if (v.start + v.len <= pos) continue;
            const SampleBuffer* src = v.file >= 0 ? plan->files[(size_t)v.file].get() : plan->bufs[(size_t)v.buf]->get();
            if (!src || src->frames <= 0) continue;
            double rate = src->sr / sr * v.speed;                // source frames a sample
            double first = std::floor(v.from * src->sr);
            double last = v.to >= 0 ? std::min((double)src->frames, std::floor(v.to * src->sr)) : (double)src->frames;
            double avail = (last - first) / rate;                // samples of the source after its start
            int64_t fade = v.fadeOut;
            if (avail > (double)v.len) fade = std::max(fade, std::max<int64_t>(1, std::min<int64_t>((int64_t)std::floor(0.02 * sr), v.len / 4)));
            int64_t k0 = std::max<int64_t>(0, pos - v.start), k1 = std::min<int64_t>(v.len, p1 - v.start);
            const float* g = &plan->gains[(size_t)v.gains];
            bool direct = src->channels == nOut_;
            for (int64_t k = k0; k < k1; ++k) {
                if ((double)k >= avail) break;
                double sp = first + (double)k * rate;
                if (sp >= last) break;
                float amp = v.gain;
                if (fade && k >= v.len - fade) amp *= (float)(v.len - 1 - k) / (float)fade;
                if (k < v.fadeIn) amp *= (float)k / (float)v.fadeIn;
                int o = i + (int)(v.start + k - pos);
                if (direct) {
                    for (int c = 0; c < nOut_; ++c) if (out[c]) out[c][o] += amp * src->interp(sp, c);
                } else {
                    float x = 0.f;
                    for (int c = 0; c < src->channels; ++c) x += src->interp(sp, c);
                    x *= amp / (float)src->channels;
                    for (int c = 0; c < nOut_; ++c) if (out[c]) out[c][o] += g[c] * x;
                }
            }
        }
        i += len;
    }
}

}  // namespace

// ---------------------------------------------------------------- what editors use (transport.h)
bool isScoreBox(const Box* b) { return dynamic_cast<const ScoreBox*>(b) != nullptr; }
std::shared_ptr<sc::Score> scoreOfBox(Box* b) {
    auto* s = dynamic_cast<ScoreBox*>(b);
    return s ? s->current() : nullptr;
}
bool replaceScoreOfBox(Box* b, std::shared_ptr<sc::Score> score) {
    auto* s = dynamic_cast<ScoreBox*>(b);
    if (!s || !score) return false;
    s->replace(std::move(score));
    return true;
}
bool scoreBoxKeptInPatch(const Box* b) {
    auto* s = dynamic_cast<const ScoreBox*>(b);
    return s && s->keptInPatch();
}
int scoreBoxView(const Box* b) {
    auto* s = dynamic_cast<const ScoreBox*>(b);
    return s ? s->openView() : (int)ScoreView::Roll;
}
void setScoreBoxView(Box* b, int view) {
    if (auto* s = dynamic_cast<ScoreBox*>(b)) s->setLastView(view);
}
RollCore* inlineRollOf(Box* b) {
    auto* s = dynamic_cast<ScoreBox*>(b);
    return s ? &s->roll() : nullptr;
}

void registerScoreObjects(ClassTable& t) {
    t.insert(t.end(), {
        {"transport", "Timing", "the patch's clock: play, stop, locate bar, loop bar1 bar2, tempo; bars, beats and ms out: [bpm] [num den]",
         with<TransportBox>()},
        {"score", "Timing", "a score that plays on the transport: notes, messages and sounds out; read/write .score: name [file.score] [channels] [@view timeline]",
         with<ScoreBox>()},
    });
}

}  // namespace moritz
