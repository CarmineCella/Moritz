// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// score_doc.h — a score being edited, without the interface: the score, where it lives, the selection, the edits
// and their undo history, and who shows it. The timeline and the roll are views of one ScoreDoc.
//
// Where a score lives:
//   Box    a score box of the open patches: the document edits the very score the box plays (scoreOfBox) and hands
//          it back after each change (replaceScoreOfBox: the box makes its plan again, the transport going on). A
//          score kept in the patch is patch memory: the patch is marked edited (patchEdited), nothing is unsaved
//          here, and its edits are steps of this history, not of the patch's (Edit > Undo with the box clicked last
//          comes here: Box::uiUndo); an undo of the patch, as any change from outside, is followed (see below).
//          A box with a file of its own (score name file.score): Save writes that file.
//   File   a .score file opened on its own (File > Open Score...): Save writes it.
//   New    a new score (File > New Score): Save asks where.
//   Sub    a score placed inside another document's score (a sub-score clip opened): an instance, shared with every
//          other placement of it. Placed from a file, it is that file's (Save writes it); embedded, its parent's
//          (the parent is marked edited and plays the change).
// One document per score: the functions that open one return the document already open for it, so two views of
// the same score show the same edits, selection and history: the timeline (timeline.h) and the roll (roll.h, through
// roll_edit.h's ScoreDocTarget: the score box's own roll, and the roll's view of the score's window, score_window.h).
//
// Edits. An Edit is a transaction of insertions, removals and replacements of events (and of the rows and the
// header): commit() makes it one undo step and tells the views; an Edit not committed (or one that failed: a cycle,
// a bad time) is taken back whole. Undo keeps only what changed: score.h's insert/remove/replace results (the event
// objects themselves, their index, their old fields). The history is bounded (undoBudget bytes and maxSteps steps;
// the oldest go first, the last one is always kept).
//
// Changes from elsewhere (a message to the box: add, clear, read; the patch's undo or code view replacing the box;
// the box deleted; the placement removed) are seen by follow(), which a view's timer calls: the document then shows
// the score as it is, and its history, which describes the old one, is cleared.
//
// GUI thread only. The audio thread never sees a ScoreDoc: it plays the plans the score box makes.
#pragma once

#include "jobs.h"
#include "objects.h"
#include "music/score.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace moritz {

namespace sc = musil::score;

class ScoreDoc : public std::enable_shared_from_this<ScoreDoc> {
public:
    enum class Where { New, File, Box, Sub };

    // ---- opening one (each returns the document already open for that score, if there is one)
    static std::shared_ptr<ScoreDoc> make(const std::string& name = "untitled");   // a new score
    static std::shared_ptr<ScoreDoc> open(const std::string& path, std::string& err);   // a .score file
    static std::shared_ptr<ScoreDoc> ofBox(Box* box);                // nullptr: not a score box
    // the score an event of parent places (a sub-score clip); nullptr when id is not a score event
    static std::shared_ptr<ScoreDoc> ofPlacement(const std::shared_ptr<ScoreDoc>& parent, uint64_t id);
    static std::shared_ptr<ScoreDoc> find(const sc::Score* s);       // the open document of s, if any
    ~ScoreDoc();

    // ---- what it is
    const sc::Score& score() const { return *score_; }
    const std::shared_ptr<sc::Score>& scorePtr() const { return score_; }
    Where where() const { return where_; }
    std::string path() const;                  // the file Save writes ("" none: new, or kept in a patch or a parent)
    std::string folder() const;                // where its relative sources are found ("" unknown)
    Box* box() const;                          // the score box now (nullptr: not bound to one, or it is gone)
    bool boxGone() const { return where_ == Where::Box && !box(); }
    const std::shared_ptr<ScoreDoc>& parent() const { return parent_; }   // Sub: the document it is placed in
    uint64_t placement() const { return placement_; }                       // Sub: the event that placed it
    std::vector<std::shared_ptr<ScoreDoc>> chain();   // from the top document down to this one (a breadcrumb)
    std::string title() const;                 // what a window shows
    uint64_t version() const { return version_; }      // grows with every change shown (edit, undo, followed)

    // ---- edits
    class Edit {
    public:
        Edit(Edit&& o) noexcept;
        Edit& operator=(Edit&&) = delete;
        ~Edit();                                     // not committed: taken back
        uint64_t insert(std::shared_ptr<sc::Event> e);   // its id (kept unless 0 or taken), 0 when refused
        bool remove(uint64_t id);
        bool replace(const sc::Event& now);           // the event of now.id gets now's fields
        void setRows(std::vector<sc::Row> rows);
        void setHeader(const std::string& name, double bpm, int num, int den);
        bool ok() const { return error_.empty(); }
        const std::string& error() const { return error_; }
        bool empty() const { return ops_.empty(); }
        // one undo step, the views told; false (taken back, the document's message says why) when an operation
        // was refused, or when there was nothing to do
        bool commit();

    private:
        friend class ScoreDoc;
        // one change and what takes it back: score.h's results kept (the event object inserted or removed and its
        // index, the old and new fields of a replaced one), or the rows or header before and after
        struct Op {
            enum Kind : uint8_t { Insert, Remove, Replace, Rows, Header } kind = Insert;
            uint64_t id = 0;
            size_t index = 0;
            std::shared_ptr<sc::Event> ev;              // Insert, Remove
            std::shared_ptr<sc::Event> before, after;   // Replace
            std::vector<sc::Row> rowsBefore, rowsAfter;
            std::string nameBefore, nameAfter;
            double bpmBefore = 60, bpmAfter = 60;
            int numBefore = 4, numAfter = 4, denBefore = 4, denAfter = 4;
            size_t bytes() const;
        };
        Edit(ScoreDoc* d, std::string name);
        ScoreDoc* doc_;
        std::string name_, error_;
        std::vector<Op> ops_;
        bool done_ = false;
    };
    Edit edit(const std::string& name);
    bool undo();
    bool redo();
    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }
    std::string undoName() const;
    std::string redoName() const;
    size_t historyBytes() const;
    size_t historySteps() const { return undo_.size() + redo_.size(); }
    static size_t undoBudget;                  // bytes the history may keep
    static size_t maxSteps;                    // steps it may keep

    // ---- the selection: event ids, in the order they were selected
    const std::vector<uint64_t>& selection() const { return sel_; }
    bool isSelected(uint64_t id) const;
    void select(std::vector<uint64_t> ids);    // ids not in the score are left out
    void toggle(uint64_t id);
    void clearSelection() { select({}); }

    // ---- saving (an atomic write through score_text.h)
    bool dirty() const;                        // edited since it was read or saved (undone back to it: not)
    bool needsSaving() const;                  // closing loses something (never a score kept in a patch)
    bool canSaveInPlace() const { return !path().empty(); }
    bool save(std::string& err);               // to path()
    bool saveAs(const std::string& path, std::string& err);   // the document is that file from then on (File, New)
    bool exportTo(const std::string& path, std::string& err) const;   // a copy

    // ---- following what changed elsewhere (true when it did: the views were told)
    bool follow();
    std::string message;                       // what the last operation did, or why it did nothing
    int lastView = -1;                         // the view its window showed last (score_window.h's ScoreView; -1 none)

    // ---- who shows it
    enum Change : unsigned { Content = 1, Selection = 2, State = 4 };   // State: title, saved, bound
    int listen(std::function<void(unsigned)> f);   // a token
    void unlisten(int token);

    // ---- playing it: the box that plays it (the bound box; otherwise a score box of its own, into dac~ 1 2, in a
    // patch run beside the others under root, made on first use and removed with the document)
    Box* player(Patch* root);

    // a score kept in a patch was edited (the application marks the patch's document edited); may be empty
    static std::function<void(Patch*)> patchEdited;
    // a patch is being destroyed (the application calls it from Patch::onDestroyed): a document bound to a box in
    // it forgets where it was, so a patch made later at the same address is never taken for it
    static void patchDestroyed(Patch* p);

private:
    struct Step {
        std::string name;
        uint64_t serial = 0;
        std::vector<Edit::Op> ops;
        size_t bytes = 0;
    };
    explicit ScoreDoc(std::shared_ptr<sc::Score> s, Where w);
    void apply(Edit::Op& op, bool forward);    // throws sc::Error
    void changed(unsigned what);               // the score changed here: played, kept, the views told
    void notify(unsigned what);
    void adopt(std::shared_ptr<sc::Score> s, const std::string& why);   // a score from elsewhere
    void trimHistory();
    void push();                               // the score to whoever plays it (and to the parents)
    void childChanged(bool embedded);
    Box* locate() const;
    static std::vector<std::weak_ptr<ScoreDoc>>& registry();

    std::shared_ptr<sc::Score> score_;
    Where where_;
    std::string path_;                         // File, New after Save As
    // Box: where it is, as the patch's undo and code view replace boxes: the root, the document's top patch (an
    // extra of the root, or the root: compared, never followed once gone) and the ids down to the box
    Patch* root_ = nullptr;
    Patch* top_ = nullptr;
    std::vector<std::string> boxPath_;
    mutable Box* box_ = nullptr;
    bool hadBox_ = false;
    // Sub
    std::shared_ptr<ScoreDoc> parent_;
    uint64_t placement_ = 0;
    bool placedGone_ = false;
    // playing
    Patch* playerRoot_ = nullptr;
    Patch* player_ = nullptr;
    // history
    std::vector<Step> undo_, redo_;
    uint64_t nextSerial_ = 1, savedSerial_ = 0;
    bool outsideEdit_ = false;                 // an embedded score placed in it was edited (unsaved, no step)
    uint64_t topSerial() const { return undo_.empty() ? 0 : undo_.back().serial; }
    uint64_t seen_ = 0;                        // the score's version when it was last pushed or followed
    uint64_t version_ = 0;
    std::vector<uint64_t> sel_;
    struct Listener {
        int token;
        std::function<void(unsigned)> f;
    };
    std::vector<Listener> listeners_;
    int nextToken_ = 1;
    bool editing_ = false;                     // an Edit is open
};

// The score rendered offline into a WAV file (its sounds, through its tracks' strips; 24 bits): a score box plays a
// copy of it on a patch of its own, run on a background job as the render object runs a patch, faster than real
// time. The patch is made and destroyed on the GUI thread; only the processing runs on the job's thread.
class ScoreRender {
public:
    ScoreRender() = default;
    ScoreRender(const ScoreRender&) = delete;
    ScoreRender& operator=(const ScoreRender&) = delete;
    ~ScoreRender();                            // a render going on is cancelled (its done() dropped)
    // folder: where its relative sources are; done(ok, message) on the GUI thread once the file is written.
    // false: not started (err says why)
    bool start(const sc::Score& s, const std::string& folder, const std::string& wav, int channels, double sr,
               std::function<void(bool, const std::string&)> done, std::string& err);
    void cancel();
    bool running() const;
    std::function<void(float)> onProgress;     // 0..1, GUI thread

private:
    std::unique_ptr<Job> job_;
    std::unique_ptr<Patch> patch_;
};

}  // namespace moritz
