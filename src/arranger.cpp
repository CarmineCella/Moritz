// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// arranger.cpp — the multitrack mode's panels around a score's timeline: the browser of files, the drawer, and the
// sessions opened as arranger windows (arranger.h)

#include "arranger.h"
#include "editor.h"
#include "fileio.h"
#include "gui.h"
#include "mixer_panel.h"
#include "multitrack_ops.h"
#include "roll.h"
#include "roll_edit.h"
#include "score_window.h"
#include "sound_editor.h"
#include "timeline.h"
#include "transport.h"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace moritz {

namespace fs = std::filesystem;
namespace tl = timeline;

namespace {
std::string baseName(const std::string& p) {
    size_t k = p.find_last_of("/\\");
    return k == std::string::npos ? p : p.substr(k + 1);
}
std::string lowerOf(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
bool listed(const std::string& name) {
    std::string l = lowerOf(name);
    for (const char* e : {".wav", ".wave", ".aif", ".aiff", ".flac", ".mp3", ".ogg", ".score", ".moritz"})
        if (l.size() > std::strlen(e) && l.compare(l.size() - std::strlen(e), std::strlen(e), e) == 0) return true;
    return false;
}
void styleButton(Fl_Button* b) {
    b->box(FL_FLAT_BOX);
    b->color(fl_rgb_color(52, 55, 63));
    b->selection_color(fl_rgb_color(70, 110, 190));
    b->labelcolor(kText);
    b->labelsize(11);
}
}  // namespace

// ================================================================ the browser

// the list: a file dragged out of it goes where it is dropped (the timeline takes paths as a Finder's drop)
class FileList : public Fl_Hold_Browser {
public:
    FileList(int x, int y, int w, int h, FileBrowserPanel* p) : Fl_Hold_Browser(x, y, w, h), panel_(p) {}
    int handle(int e) override {
        if (e == FL_PUSH) {
            pressX_ = Fl::event_x();
            pressY_ = Fl::event_y();
            dragged_ = false;
        }
        if (e == FL_DRAG && !dragged_ && (std::abs(Fl::event_x() - pressX_) > 6 || std::abs(Fl::event_y() - pressY_) > 6)) {
            int line = value();
            std::string path = line > 0 ? panel_->pathOf(line) : std::string();
            if (!path.empty() && listed(path)) {
                dragged_ = true;
                Fl::copy(path.c_str(), (int)path.size(), 0);
                Fl::dnd();
                return 1;
            }
        }
        if (e == FL_RELEASE && !dragged_ && Fl::event_clicks()) {
            int r = Fl_Hold_Browser::handle(e);
            panel_->openLine(value());
            return r;
        }
        return Fl_Hold_Browser::handle(e);
    }

private:
    FileBrowserPanel* panel_;
    int pressX_ = 0, pressY_ = 0;
    bool dragged_ = false;
};

FileBrowserPanel::FileBrowserPanel(int x, int y, int w, int h) : Fl_Group(x, y, w, h) {
    box(FL_FLAT_BOX);
    color(fl_rgb_color(30, 31, 36));
    title_ = new Fl_Box(x + 8, y + 2, w - 16, 20, "FILES");
    title_->labelsize(10);
    title_->labelfont(FL_HELVETICA_BOLD);
    title_->labelcolor(kDim);
    title_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    up_ = new Fl_Button(x + w - 64, y + 3, 26, 18, "@8->");
    up_->tooltip("The folder above");
    choose_ = new Fl_Button(x + w - 34, y + 3, 26, 18, "...");
    choose_->tooltip("Another folder");
    for (Fl_Button* b : {up_, choose_}) styleButton(b);
    up_->callback([](Fl_Widget*, void* d) {
        auto* p = static_cast<FileBrowserPanel*>(d);
        fs::path dir = fs::u8path(p->dir_).parent_path();
        if (!dir.empty()) p->setFolder(dir.u8string());
    }, this);
    choose_->callback([](Fl_Widget*, void* d) {
        auto* p = static_cast<FileBrowserPanel*>(d);
        Fl_Native_File_Chooser fc;
        fc.title("A folder of sounds");
        fc.type(Fl_Native_File_Chooser::BROWSE_DIRECTORY);
        if (fc.show() == 0) p->setFolder(portablePath(fc.filename()));
    }, this);
    path_ = new Fl_Box(x + 8, y + 22, w - 16, 18);
    path_->labelsize(10);
    path_->labelcolor(fl_rgb_color(150, 190, 250));
    path_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
    list_ = new FileList(x + 4, y + 42, w - 8, h - 46, this);
    list_->color(fl_rgb_color(26, 27, 31));
    list_->textcolor(kText);
    list_->textsize(12);
    list_->selection_color(fl_rgb_color(60, 90, 150));
    list_->box(FL_FLAT_BOX);
    list_->tooltip("Drag a sound into a track; a double-click adds it at the playhead on the current track");
    end();
    resizable(list_);
}

void FileBrowserPanel::resize(int x, int y, int w, int h) {
    Fl_Widget::resize(x, y, w, h);
    title_->resize(x + 8, y + 2, w - 16, 20);
    up_->resize(x + w - 64, y + 3, 26, 18);
    choose_->resize(x + w - 34, y + 3, 26, 18);
    path_->resize(x + 8, y + 22, w - 16, 18);
    list_->resize(x + 4, y + 42, w - 8, std::max(20, h - 46));
}

void FileBrowserPanel::setFolder(const std::string& dir) {
    dir_ = dir;
    path_->copy_label(baseName(dir).c_str());
    path_->copy_tooltip(dir.c_str());
    refresh();
}

void FileBrowserPanel::refresh() {
    names_.clear();
    std::vector<std::string> dirs, files;
    std::error_code ec;
    for (fs::directory_iterator it(fs::u8path(dir_), ec), end; !ec && it != end; it.increment(ec)) {
        std::string n = it->path().filename().u8string();
        if (n.empty() || n[0] == '.') continue;
        std::error_code e2;
        if (it->is_directory(e2)) dirs.push_back(n + "/");
        else if (listed(n)) files.push_back(n);
    }
    std::sort(dirs.begin(), dirs.end());
    std::sort(files.begin(), files.end());
    list_->clear();
    for (auto* v : {&dirs, &files})
        for (auto& n : *v) {
            names_.push_back(n);
            std::string shown = n.back() == '/' ? "@C" + std::to_string((int)fl_rgb_color(150, 160, 180)) + "@." + n : "@." + n;
            list_->add(shown.c_str());
        }
    list_->redraw();
}

std::vector<std::string> FileBrowserPanel::entries() const { return names_; }

std::string FileBrowserPanel::pathOf(int line) const {
    if (line < 1 || line > (int)names_.size()) return "";
    std::string n = names_[(size_t)line - 1];
    if (!n.empty() && n.back() == '/') n.pop_back();
    return (fs::u8path(dir_) / fs::u8path(n)).u8string();
}

void FileBrowserPanel::openLine(int line) {
    if (line < 1 || line > (int)names_.size()) return;
    std::string path = pathOf(line);
    if (names_[(size_t)line - 1].back() == '/') setFolder(path);
    else if (onOpen) onOpen(path);
}

// ================================================================ the drawer

// a patch edited in the drawer: no history of its own (its file is written by Save), its messages to the window
class DrawerEditor : public PatchEditor {
public:
    explicit DrawerEditor(Drawer* d) : d_(d) {}
    bool editable() const override { return true; }
    void pushUndo(Patch*) override {}
    void dropUndo() override {}
    void markDirty(Patch*) override {
        d_->dirtyPatch_ = true;
        d_->status("the patch is edited (it plays as it is now): Save writes it to its file");
    }
    void print(const std::string& s) override { App::get().print(s); }
    void setStatus(const std::string& s) override { d_->status(s); }
    void lockChanged() override {}
    void openSubpatch(Box*) override { d_->status("a subpatch opens from the patch's own window (File > Open)"); }

private:
    Drawer* d_;
};

namespace {
std::vector<Drawer*>& drawers() {
    static std::vector<Drawer*> v;
    return v;
}
void watchPatches() {                            // a patch destroyed: a drawer's canvas showing it lets go
    static bool installed = false;
    if (installed) return;
    installed = true;
    auto before = Patch::onDestroyed;
    Patch::onDestroyed = [before](Patch* p) {
        for (Drawer* d : drawers()) d->patchDestroyed(p);
        if (before) before(p);
    };
}
}  // namespace

Drawer::Drawer(int x, int y, int w, int h, std::shared_ptr<ScoreDoc> doc, std::shared_ptr<ScorePlayback> play)
    : Fl_Group(x, y, w, h), doc_(std::move(doc)), play_(std::move(play)) {
    watchPatches();
    drawers().push_back(this);
    box(FL_FLAT_BOX);
    color(fl_rgb_color(24, 25, 29));
    bar_ = new Fl_Group(x, y, w, 26);
    bar_->box(FL_FLAT_BOX);
    bar_->color(fl_rgb_color(32, 34, 40));
    const char* names[] = {"Clip", "Track", "Mixer"};
    const char* tips[] = {"The selected clip's editor: a sound, notes, a placed score, a patch clip",
                          "The current track's patch: its insert or its instrument, edited while it plays", "The mixer"};
    for (int k = 0; k < 3; ++k) {
        tabs_[k] = new Fl_Button(x + 6 + 74 * k, y + 3, 70, 20, names[k]);
        styleButton(tabs_[k]);
        tabs_[k]->type(FL_TOGGLE_BUTTON);
        tabs_[k]->tooltip(tips[k]);
        tabs_[k]->callback([](Fl_Widget* w, void* d) {
            auto* dr = static_cast<Drawer*>(d);
            for (int j = 0; j < 3; ++j)
                if (dr->tabs_[j] == w) dr->showTab((Tab)j);
        }, this);
    }
    tools_ = new Fl_Group(x + 240, y, w - 240, 26);
    const char* tl[] = {"Insert", "Instrument", "Choose...", "New", "None", "Save", "Edit"};
    int bx = x + 248;
    for (int k = 0; k < 7; ++k) {
        auto* b = new Fl_Button(bx, y + 3, k < 2 ? 78 : 62, 20, tl[k]);
        styleButton(b);
        if (k < 2 || k == 6) b->type(FL_TOGGLE_BUTTON);
        b->callback([](Fl_Widget* w, void* d) {
            auto* dr = static_cast<Drawer*>(d);
            std::string l = w->label();
            ScoreDoc& doc = *dr->doc_;
            if (l == "Insert" || l == "Instrument") dr->setRole(l == "Insert" ? "insert" : "instrument");
            else if (l == "Choose...") {
                if (dr->track_.empty() && tl::tracks(doc.score()).empty()) { dr->status("no track"); return; }
                std::string f = chooseFile(false, "A patch: inlet~ to outlet~ (insert), or notes in, sound out (instrument)", "Patch\t*.moritz");
                if (!f.empty()) {
                    mt::setTrackPatch(doc, dr->track_, dr->role_, f);
                    dr->status(doc.message);
                    dr->rebuild();
                }
            } else if (l == "New") dr->newTrackPatch();
            else if (l == "None") {
                mt::setTrackPatch(doc, dr->track_, dr->role_, "");
                dr->rebuild();
            } else if (l == "Save") dr->saveTrackPatch();
            else if (l == "Edit" && dr->canvas_) dr->canvas_->setLocked(static_cast<Fl_Button*>(w)->value() == 0);
        }, this);
        bx += (k < 2 ? 78 : 62) + 4;
    }
    tools_->end();
    auto* spacer = new Fl_Box(x + w - 2, y, 1, 26);
    bar_->resizable(spacer);
    bar_->end();
    content_ = new Fl_Group(x, y + 26, w, h - 26);
    content_->end();
    end();
    resizable(content_);
    auto tracks = tl::tracks(doc_->score());
    if (!tracks.empty()) track_ = tracks.front();
    showTab(Tab::Mixer);
}

void Drawer::showTrackLater(void* d) { static_cast<Drawer*>(d)->showTab(Tab::Track); }

Drawer::~Drawer() {
    Fl::remove_timeout(showTrackLater, this);
    auto& v = drawers();
    v.erase(std::remove(v.begin(), v.end(), this), v.end());
    if (canvas_) canvas_->detach();
}

void Drawer::resize(int x, int y, int w, int h) {
    Fl_Widget::resize(x, y, w, h);
    bar_->resize(x, y, w, 26);
    int bx = x + 6;
    for (auto* t : tabs_) { t->resize(bx, y + 3, 70, 20); bx += 74; }
    int tx = x + 248;
    for (int k = 0; k < tools_->children(); ++k) {
        Fl_Widget* c = tools_->child(k);
        c->resize(tx, y + 3, c->w(), 20);
        tx += c->w() + 4;
    }
    content_->resize(x, y + 26, w, h - 26);
    for (int k = 0; k < content_->children(); ++k) content_->child(k)->resize(x, y + 26, w, h - 26);
}

void Drawer::showTab(Tab t) {
    if (t != tab_ && !confirmLeave()) {
        for (int j = 0; j < 3; ++j) tabs_[j]->value(j == (int)tab_);
        return;
    }
    tab_ = t;
    for (int j = 0; j < 3; ++j) tabs_[j]->value(j == (int)t);
    if (t == Tab::Track) tools_->show();
    else tools_->hide();
    rebuild();
}

void Drawer::setTrack(const std::string& track) {
    if (track == track_) return;
    if (tab_ == Tab::Track && !confirmLeave()) return;
    track_ = track;
    if (tab_ == Tab::Track) rebuild();
}

void Drawer::setRole(const std::string& role) {
    if (role != role_ && tab_ == Tab::Track && !confirmLeave()) return;
    role_ = role;
    if (tab_ == Tab::Track) rebuild();
    else showTab(Tab::Track);
}

void Drawer::selectionChanged() {
    uint64_t c = 0;
    for (uint64_t id : doc_->selection())
        if (auto e = doc_->score().find(id); e && tl::isClip(*e)) { c = id; break; }
    if (c == clip_) return;
    if (!confirmLeave()) return;
    clip_ = c;
    if (tab_ == Tab::Clip) rebuild();
}

void Drawer::contentChanged() {
    if (tab_ != Tab::Track) return;
    Box* box = doc_->playerBox();
    Patch* p = box ? mt::trackPatch(box, track_, role_) : nullptr;
    if (p != shownPatch_ || (p && p->file != shownFile_)) rebuild();   // another patch, or none: shown again
}

bool Drawer::confirmLeave() {
    if (dirtyPatch_ && canvas_ && canvas_->patch() && !canvas_->patch()->file.empty()) {   // a patch edited here
        int a = askUser("Save the changes to the patch \"%s\"?", "Save", "Cancel", "Don't Save", baseName(canvas_->patch()->file).c_str());
        if (a == 1) return false;
        if (a == 0 && !saveTrackPatch()) return false;
        dirtyPatch_ = false;
    }
    if (!sound_ || !soundDoc_ || !soundDoc_->needsSaving()) return true;
    int a = askUser("Save the changes to the sound \"%s\"?", "Save", "Cancel", "Don't Save", baseName(soundDoc_->path()).c_str());
    if (a == 1) return false;
    if (a == 0) {
        std::string err;
        if (!soundDoc_->save(err)) { alertUser("Can't save the sound:\n%s", err.c_str()); return false; }
    }
    return true;
}

void Drawer::clearContent() {
    if (canvas_) canvas_->detach();
    while (content_->children() > 0) {
        Fl_Widget* w = content_->child(0);
        content_->remove(w);
        Fl::delete_widget(w);                    // later: an event of its own may be what got us here
    }
    mixer_ = nullptr;
    canvas_ = nullptr;
    sound_ = nullptr;
    roll_ = nullptr;
    note_ = nullptr;
    editor_.reset();
    dirtyPatch_ = false;
}

void Drawer::rebuild() {
    clearContent();
    int X = content_->x(), Y = content_->y(), W = content_->w(), H = content_->h();
    content_->begin();
    auto note = [&](const std::string& t) {
        note_ = new Fl_Box(X, Y, W, H);
        note_->copy_label(t.c_str());
        note_->labelcolor(kDim);
        note_->labelsize(12);
        note_->box(FL_FLAT_BOX);
        note_->color(fl_rgb_color(24, 25, 29));
    };
    Box* box = doc_->playerBox();
    shown_.clear();
    switch (tab_) {
    case Tab::Mixer: {
        mixer_ = new MixerPanel(X, Y, W, H, doc_);
        mixer_->onStatus = [this](const std::string& s) { status(s); };
        mixer_->onTrack = [this](const std::string& t) { setTrack(t); };
        mixer_->onInsert = [this](const std::string& t) {
            track_ = t;
            role_ = "insert";
            Fl::add_timeout(0, showTrackLater, this);   // not from inside the mixer's own event (it goes)
        };
        shown_ = "mixer";
        break;
    }
    case Tab::Track: {
        for (int k = 0; k < tools_->children(); ++k) {
            auto* b = static_cast<Fl_Button*>(tools_->child(k));
            std::string l = b->label();
            if (l == "Insert") b->value(role_ == "insert");
            if (l == "Instrument") b->value(role_ == "instrument");
        }
        Patch* p = box ? mt::trackPatch(box, track_, role_) : nullptr;
        shownPatch_ = p;
        shownFile_ = p ? p->file : std::string();
        if (p) {
            editor_ = std::make_unique<DrawerEditor>(this);
            canvas_ = new Canvas(X, Y, W, H, editor_.get(), p);
            canvas_->setLocked(true);
            shown_ = "track " + track_ + " " + role_ + " " + baseName(p->file);
        } else {
            note(track_.empty() ? "no track: add one to give it an insert or an instrument"
                                : "\"" + track_ + "\" has no " + role_ + ": Choose... a patch, or New makes one next to the session");
            shown_ = "track " + track_ + " " + role_ + " none";
        }
        break;
    }
    case Tab::Clip: {
        auto e = clip_ ? doc_->score().find(clip_) : nullptr;
        if (!e) {
            note("select a clip: its editor shows here");
            shown_ = "clip none";
            break;
        }
        if (e->kind == sc::Kind::Sound && !e->buffer) {
            std::string path = tl::sourcePath(*doc_, e->source);
            if (!soundDoc_ || soundDoc_->path() != path) {
                auto sd = std::make_shared<SoundDoc>();
                std::string err;
                if (!sd->load(path, err)) {
                    note("can't read " + baseName(path) + ": " + err);
                    shown_ = "clip sound unreadable";
                    break;
                }
                soundDoc_ = sd;
            }
            sound_ = new SoundEditor(X, Y, W, H, soundDoc_);
            if (H < 420) {                               // a drawer is low: the waveform first, the regions' table hidden
                sound_->bottomPanel()->hide();
                sound_->resize(X, Y, W, H);
            }
            double from = std::max(0.0, e->from), to = e->to >= 0 ? e->to : from + e->dur;
            long a = (long)std::lround(from * soundDoc_->sr()), b = (long)std::lround(to * soundDoc_->sr());
            soundDoc_->select(a, b);
            sound_->selectionChanged();
            shown_ = "clip sound " + baseName(path);
        } else if (e->kind == sc::Kind::Sound) {
            note("a buffer~'s sound: a double-click on the clip opens its editor");
            shown_ = "clip buffer " + e->source;
        } else if (e->kind == sc::Kind::Note) {
            roll_ = new RollPanel(X, Y, W, H, makeRollTarget(doc_), play_);
            shown_ = "clip notes";
        } else if (e->kind == sc::Kind::Score) {
            auto sub = ScoreDoc::ofPlacement(doc_, clip_);
            if (sub) {
                roll_ = new RollPanel(X, Y, W, H, makeRollTarget(sub), nullptr);
                shown_ = "clip score " + sub->title();
            }
        } else if (e->kind == sc::Kind::Patch) {
            Patch* p = box ? mt::clipPatch(box, clip_) : nullptr;
            if (p) {
                editor_ = std::make_unique<DrawerEditor>(this);
                canvas_ = new Canvas(X, Y, W, H, editor_.get(), p);
                canvas_->setLocked(true);
                shown_ = "clip patch " + baseName(p->file);
            } else {
                note("the patch clip's file can't be read");
                shown_ = "clip patch none";
            }
        }
        break;
    }
    }
    content_->end();
    content_->redraw();
}

std::string Drawer::describe() const { return "drawer " + shown_; }

void Drawer::patchDestroyed(Patch* p) {
    if (canvas_ && canvas_->patch() == p) {
        canvas_->detach();
        canvas_->hide();
    }
}

bool Drawer::saveTrackPatch() {
    Patch* p = canvas_ ? canvas_->patch() : nullptr;
    if (!p || p->file.empty()) { status("no patch to save"); return false; }
    std::string err;
    if (!fileio::writeAtomic(p->file, p->serialize(), &err)) {
        alertUser("Can't save %s:\n%s", baseName(p->file).c_str(), err.c_str());
        return false;
    }
    dirtyPatch_ = false;
    status("saved " + p->file);
    return true;
}

bool Drawer::newTrackPatch() {
    if (track_.empty()) { status("no track"); return false; }
    std::string dir = doc_->folder();
    if (dir.empty() || doc_->path().empty()) { status("save the session first: the patch goes next to it"); return false; }
    std::string base = track_ + " " + role_;
    for (auto& c : base) if (c == '/' || c == '\\' || c == ':') c = '_';
    std::string path;
    for (int n = 1; n < 1000; ++n) {
        path = dir + "/" + base + (n > 1 ? " " + std::to_string(n) : std::string()) + ".moritz";
        if (!fileio::exists(path)) break;
    }
    // an insert: the track through as it is (put effects between); an instrument: a note's click into both sides
    std::string text = role_ == "insert"
        ? "(patch\n  (obj i1 inlet~ 1 (pos 40 30))\n  (obj i2 inlet~ 2 (pos 160 30))\n  (obj o1 outlet~ 1 (pos 40 160))\n"
          "  (obj o2 outlet~ 2 (pos 160 160))\n  (obj c comment \"the track's audio: put effects between\" (pos 240 90))\n"
          "  (connect i1 0 o1 0)\n  (connect i2 0 o2 0))\n"
        : "(patch\n  (obj in inlet (pos 40 30))\n  (obj c comment \"note pitch velocity ms: a synth here (or poly~ for voices)\" (pos 160 30))\n"
          "  (obj r route note (pos 40 70))\n  (obj u unpack 0 0 0 (pos 40 110))\n  (obj t t b f (pos 40 150))\n"
          "  (obj m mtof (pos 120 190))\n  (obj s cycle~ (pos 120 230))\n  (obj e message \"0.2 5, 0 400\" (pos 40 190))\n"
          "  (obj l line~ (pos 40 230))\n  (obj x *~ (pos 40 270))\n  (obj o1 outlet~ 1 (pos 40 320))\n  (obj o2 outlet~ 2 (pos 160 320))\n"
          "  (connect in 0 r 0)\n  (connect r 0 u 0)\n  (connect u 0 t 0)\n  (connect t 1 m 0)\n  (connect m 0 s 0)\n"
          "  (connect t 0 e 0)\n  (connect e 0 l 0)\n  (connect l 0 x 0)\n  (connect s 0 x 1)\n  (connect x 0 o1 0)\n  (connect x 0 o2 0))\n";
    std::string err;
    if (!fileio::writeAtomic(path, text, &err)) { status("can't write " + path + ": " + err); return false; }
    bool ok = mt::setTrackPatch(*doc_, track_, role_, path);
    status(ok ? "made " + baseName(path) : doc_->message);
    rebuild();
    return ok;
}

// ================================================================ sessions

bool isSession(const sc::Score& s) { return sc::numField(s.extra, "session", 0) != 0; }

ScoreWindow* newSession() {
    auto d = ScoreDoc::make("session");
    {
        auto s = d->score().clone();
        sc::setField(s->extra, "session", sc::Datum::number(1));
        mt::SessionSettings v = mt::sessionSettings(*s);
        v.crossfade = true;
        mt::setSessionSettings(*s, v);
        auto e = d->edit("New Session");
        e.setExtra(s->extra);
        std::vector<sc::Row> rows{sc::Row{"audio 1", "", {}}, sc::Row{"audio 2", "", {}}};
        e.setRows(rows);
        e.commit();
    }
    return openScore(d, (int)ScoreView::Timeline, true);
}

ScoreWindow* openSession(const std::string& given) {
    std::string path = fileio::absolute(given);
    std::string err;
    auto d = ScoreDoc::open(path, err);
    if (!d) {
        alertUser("Can't open %s:\n%s", baseName(path).c_str(), err.c_str());
        return nullptr;
    }
    App::get().addRecent(path);
    return openScore(d, (int)ScoreView::Timeline, true);
}

void openSessionDialog() {
    Fl_Native_File_Chooser fc;
    fc.title("Open Session");
    fc.type(Fl_Native_File_Chooser::BROWSE_FILE);
    fc.filter("Session (a score)\t*.score\nAll files\t*");
    if (fc.show() != 0) return;
    openSession(portablePath(fc.filename()));
}

// ================================================================ the test actions (guitest.cpp): act=mt...
//   act=mtnew                 File > New Session
//   act=mtopen:<file>         a .score file opened as a session
//   act=mtstate               print: the front arranger (its title, its tracks with their settings, the drawer, whether
//                             it records, the plans its player made)
//   act=mtarm:<track>         a click on the track's R; act=mtmonitor:<track> on its I
//   act=mtinput:<track>,<in>,<channels>   the track's input (as its menu sets it)
//   act=mtvolume:<track>,<dB> the volume in the track's header dragged there (heard during the drag; one step at the end)
//   act=mttab:clip|track|mixer   the drawer's tab; act=mtdrawer: print what it shows
//   act=mtrole:insert|instrument, act=mtnewpatch, act=mtsavepatch   the drawer's Track tab
//   act=mtmixer               print: every strip of the mixer
//   act=mtfader:<name>,<dB>   the mixer's fader of a track (master: @master) dragged to dB, the live value printed
//                             during the drag
//   act=mtbrowse:<folder>     the browser's folder; act=mtbrowser: print its entries; act=mtbrowseopen:<line>
//   act=mtrecord:<seconds>    Record with a fake device (no audio running): the input a sine, the session run for that
//                             long, then Stop; the takes are clips
//   act=mtselect:<id>         a clip selected (the drawer's Clip tab follows)
namespace {
ScoreWindow* frontArranger() {
    if (auto* w = frontScore(); w && w->arranger()) return w;
    for (auto it = scoreWindows().rbegin(); it != scoreWindows().rend(); ++it)
        if ((*it)->arranger()) return *it;
    return nullptr;
}
void sayState(const std::string& s) {
    std::printf("state: %s\n", s.c_str());
    std::fflush(stdout);
}
std::vector<std::string> splitArgs(const std::string& s) {
    std::vector<std::string> out;
    size_t k = 0;
    for (;;) {
        size_t e = s.find(',', k);
        out.push_back(s.substr(k, e == std::string::npos ? std::string::npos : e - k));
        if (e == std::string::npos) break;
        k = e + 1;
    }
    return out;
}
void fakeMouse(Fl_Widget* v, int ex, int ey, int event, int clicks) {
    Fl::e_x = Fl::e_x_root = ex;
    Fl::e_y = Fl::e_y_root = ey;
    Fl::e_state = event == FL_RELEASE ? 0 : FL_BUTTON1;
    Fl::e_clicks = clicks;
    Fl::e_is_click = 1;
    Fl::e_keysym = FL_Button + FL_LEFT_MOUSE;
    Fl::e_number = event;
    v->handle(event);
}
const TimelineView::Row* rowOfTrack(TimelineView* v, const std::string& track) {
    for (auto& r : v->rows())
        if (r.track == track && r.lane < 0) return &r;
    return nullptr;
}
}  // namespace

bool arrangerTestAction(const std::string& verb, const std::string& arg) {
    if (verb == "mtnew") return newSession() != nullptr;
    if (verb == "mtopen") return openSession(arg) != nullptr;
    ScoreWindow* w = frontArranger();
    if (!w) { sayState(verb + " no arranger"); return verb == "mtstate"; }
    TimelinePanel* p = w->timeline();
    TimelineView* v = p->view();
    ScoreDoc& d = p->doc();
    Drawer* dr = w->drawer();
    Box* player = d.playerBox();
    if (verb == "mtstate") {
        sayState("arranger " + std::string(w->label() ? w->label() : "") + (isSession(d.score()) ? " session" : ""));
        for (auto& t : tl::tracks(d.score())) {
            mt::RowSettings rs = mt::settingsOf(d.score(), t);
            sc::Strip st = tl::stripOf(d.score(), t);
            sayState("arranger track " + t + (rs.bus ? " bus" : "") + " arm " + (rs.arm ? "yes" : "no") + " input " +
                     std::to_string(rs.input) + "/" + std::to_string(rs.inputCh) + " monitor " + (rs.monitor ? "yes" : "no") +
                     " gain " + mt::dbText(st.gain) + (rs.insert.empty() ? "" : " insert " + baseName(rs.insert)) +
                     (rs.instrument.empty() ? "" : " instrument " + baseName(rs.instrument)));
        }
        int clips = 0;
        for (auto& e : d.score().events()) clips += tl::isClip(*e);
        sayState("arranger clips " + std::to_string(clips) + " recording " + (w->recording() ? "yes" : "no") + " " + dr->describe());
        if (player) sayState("arranger plans " + std::to_string(mt::planInfo(player).plans));
        sayState("arranger status " + w->status());
        return true;
    }
    if (verb == "mtarm" || verb == "mtmonitor") {
        const TimelineView::Row* r = rowOfTrack(v, arg);
        if (!r) return false;
        int by = v->rowTop(*r) + std::min(8, std::max(0, (r->h - 17) / 2)) + 6;
        int ex = v->x() + v->headW() - 104 + (verb == "mtarm" ? 44 : 66) + 8;
        v->press(ex, by, 0, false, false);
        v->release(ex, by);
        return true;
    }
    if (verb == "mtinput") {
        auto a = splitArgs(arg);
        if (a.size() < 3) return false;
        mt::RowSettings rs = mt::settingsOf(d.score(), a[0]);
        rs.input = std::atoi(a[1].c_str());
        rs.inputCh = std::atoi(a[2].c_str());
        return mt::setSettings(d, a[0], rs, "Input");
    }
    if (verb == "mtvolume") {
        auto a = splitArgs(arg);
        const TimelineView::Row* r = a.size() == 2 ? rowOfTrack(v, a[0]) : nullptr;
        if (!r) return false;
        int ly, in0, in1, v0, v1, p0, p1;
        v->sessionLine(*r, ly, in0, in1, v0, v1, p0, p1);
        double db = std::atof(a[1].c_str());
        int from = v0 + (int)std::lround((mt::gainToDb(tl::stripOf(d.score(), a[0]).gain) + 60) / 72 * (v1 - v0));
        int to = v0 + (int)std::lround((db + 60) / 72 * (v1 - v0));
        std::string track = a[0];
        v->press(from, ly, 0, false, false);
        v->dragTo(to, ly);
        if (player) sayState("arranger live " + track + " " + mt::dbText(mt::stripOf(player, track)->gain.load()));
        v->release(to, ly);
        return true;
    }
    if (verb == "mttab") {
        dr->showTab(arg == "clip" ? Drawer::Tab::Clip : arg == "track" ? Drawer::Tab::Track : Drawer::Tab::Mixer);
        return true;
    }
    if (verb == "mtdrawer") { sayState(dr->describe()); return true; }
    if (verb == "mtrole") { dr->setRole(arg); return true; }
    if (verb == "mtnewpatch") return dr->newTrackPatch();
    if (verb == "mtsavepatch") return dr->saveTrackPatch();
    if (verb == "mtmixer" || verb == "mtfader") {
        if (dr->tab() != Drawer::Tab::Mixer) dr->showTab(Drawer::Tab::Mixer);
        MixerPanel* m = dr->mixer();
        if (!m) return false;
        m->layoutStrips();
        if (verb == "mtmixer") {
            for (auto& s : m->strips()) sayState("mixer " + m->describe(s.name));
            return true;
        }
        auto a = splitArgs(arg);
        if (a.size() != 2) return false;
        for (auto& s : m->strips()) {
            if (s.name != a[0]) continue;
            std::string name = s.name;
            int y0 = m->fyOf(s, m->gainOf(name)), y1 = m->fyOf(s, mt::dbToGain(std::atof(a[1].c_str())));
            int x = s.x + 22;
            fakeMouse(m, x, y0, FL_PUSH, 0);
            fakeMouse(m, x, (y0 + y1) / 2, FL_DRAG, 0);
            fakeMouse(m, x, y1, FL_DRAG, 0);
            if (player) sayState("mixer live " + name + " " + mt::dbText(mt::stripOf(player, name)->gain.load()));
            fakeMouse(m, x, y1, FL_RELEASE, 0);
            return true;
        }
        return false;
    }
    if (verb == "mtbrowse") { w->browser()->setFolder(arg); return true; }
    if (verb == "mtbrowser") {
        std::string t;
        for (auto& e : w->browser()->entries()) t += e + "|";
        sayState("browser " + t);
        return true;
    }
    if (verb == "mtbrowseopen") { w->browser()->openLine(std::atoi(arg.c_str())); return true; }
    if (verb == "mtselect") {
        d.select({(uint64_t)std::atoll(arg.c_str())});
        return true;
    }
    if (verb == "mtrecord") {
        App& app = App::get();
        if (app.audio.running()) { sayState("arranger record: audio is on (the fake device needs it off)"); return false; }
        if (!w->toggleRecord(0)) return false;
        Patch& host = app.host;
        double sr = host.sampleRate.load();
        long frames = (long)(std::atof(arg.c_str()) * sr);
        std::vector<float> in(64), out(2 * 64);
        for (long done = 0; done < frames; done += 64) {
            for (int i = 0; i < 64; ++i) in[(size_t)i] = 0.25f * (float)std::sin(2 * 3.14159265358979 * 440 * (double)(done + i) / sr);
            host.runDue(host.time());
            host.process(in.data(), 1, out.data(), 2, 64);
        }
        w->toggleRecord(0);
        return true;
    }
    return false;
}

}  // namespace moritz
