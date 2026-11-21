// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// arranger.h — the multitrack mode's window (mockup 8, layout B): a session is a .score file opened in a score's window
// laid out as a DAW. The score window (score_window.h) is the same, with the panels around its timeline: a browser of
// files on the left (sounds dragged into the tracks, or added at the playhead by a double-click), the timeline in the
// middle with a session's track headers (arm, input, monitoring, volume, pan, a meter) and its inspector on the right,
// and the drawer at the bottom (mockup 4): the selected clip's editor (a sound: the sound editor on its region; notes
// or a placed score: the roll; a patch clip: its patch, live), the current track's patch (its insert or instrument,
// edited live while the piece plays), or the mixer. The transport bar gets Record: the armed tracks record from the
// playhead (multitrack_ops.h's SessionRecorder), their takes added as clips when it stops.
//
// File > New Session and File > Open Session... open one; a .score file whose data says it is a session (session 1,
// written by New Session) opens so from Open Score... too.
#pragma once

#include "score_doc.h"

#include <FL/Fl_Group.H>

#include <functional>
#include <memory>
#include <string>
#include <vector>

class Fl_Box;
class Fl_Hold_Browser;
class Fl_Button;

namespace moritz {

class Canvas;
class MixerPanel;
class RollPanel;
class ScorePlayback;
class ScoreWindow;
class SoundDoc;
class SoundEditor;
class DrawerEditor;
class FileList;

// the files of a folder: sounds, scores and patches, and the folders in it
class FileBrowserPanel : public Fl_Group {
public:
    FileBrowserPanel(int x, int y, int w, int h);
    void setFolder(const std::string& dir);
    const std::string& folder() const { return dir_; }
    void refresh();
    std::vector<std::string> entries() const;                // the names listed (folders end with /)
    std::string pathOf(int line) const;                       // a line's file (1-based, as the browser's)
    void openLine(int line);                                  // a double-click: into a folder, or onOpen a file
    std::function<void(const std::string& path)> onOpen;
    void resize(int x, int y, int w, int h) override;

private:
    std::string dir_;
    Fl_Box* title_;
    Fl_Box* path_;
    Fl_Button* up_;
    Fl_Button* choose_;
    FileList* list_;
    std::vector<std::string> names_;
};

// the drawer: a clip's editor, the current track's patch, or the mixer
class Drawer : public Fl_Group {
public:
    enum class Tab { Clip = 0, Track = 1, Mixer = 2 };
    Drawer(int x, int y, int w, int h, std::shared_ptr<ScoreDoc> doc, std::shared_ptr<ScorePlayback> play);
    ~Drawer() override;
    Tab tab() const { return tab_; }
    void showTab(Tab t);
    void selectionChanged();                     // the Clip tab shows the selected clip
    void setTrack(const std::string& track);     // the Track tab's track
    const std::string& track() const { return track_; }
    void setRole(const std::string& role);       // the Track tab shows the insert or the instrument
    const std::string& role() const { return role_; }
    MixerPanel* mixer() const { return mixer_; }
    Canvas* canvas() const { return canvas_; }
    SoundEditor* soundEditor() const { return sound_; }
    RollPanel* roll() const { return roll_; }
    std::string describe() const;                // what it shows (the tests print it)
    bool confirmLeave();                         // the sound shown has edits: asks to save them; false: cancel
    bool saveTrackPatch();                       // the patch shown written to its file
    bool newTrackPatch();                        // a new insert (inlet~ to outlet~) or instrument next to the session
    void patchDestroyed(Patch* p);               // a patch is going: a canvas showing it lets go
    std::function<void(const std::string&)> onStatus;
    void resize(int x, int y, int w, int h) override;

private:
    friend class DrawerEditor;
    static void showTrackLater(void* d);
    void rebuild();                              // the content for the tab and what it shows
    void clearContent();
    void status(const std::string& s) { if (onStatus) onStatus(s); }
    std::shared_ptr<ScoreDoc> doc_;
    std::shared_ptr<ScorePlayback> play_;
    Tab tab_ = Tab::Mixer;
    std::string track_, role_ = "insert";
    uint64_t clip_ = 0;
    Fl_Group* bar_;
    Fl_Button* tabs_[3];
    Fl_Group* tools_;                            // the Track tab's buttons
    Fl_Group* content_;
    Fl_Box* note_ = nullptr;
    MixerPanel* mixer_ = nullptr;
    Canvas* canvas_ = nullptr;
    SoundEditor* sound_ = nullptr;
    RollPanel* roll_ = nullptr;
    std::shared_ptr<SoundDoc> soundDoc_;
    std::unique_ptr<DrawerEditor> editor_;
    std::string shown_;                          // what the content shows (describe)
    bool dirtyPatch_ = false;
};

// ---- the application's side
ScoreWindow* newSession();                       // File > New Session
ScoreWindow* openSession(const std::string& path);   // a .score file as a session
void openSessionDialog();                        // File > Open Session...
bool isSession(const sc::Score& s);              // its data says so (session 1)

// the test actions (guitest.cpp): act=mt...
bool arrangerTestAction(const std::string& verb, const std::string& arg);

}  // namespace moritz
