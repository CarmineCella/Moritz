// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// roll_edit.h — what the roll asks of the score it edits: edit intents (move, resize, copy, insert, remove, set a
// field, transform, replace the whole score), each one undo step, on the score at a path of sub-scores.
//
// The roll (roll.h) never changes a score itself: it names what the user did, and a RollEditTarget does it. The
// intents are written on one primitive, apply(): a function that changes a copy of the score at a path; the copy is
// put back in its place (the scores placed on the way down copied too, every instance of the one edited following,
// score_transform.h's replaceInstance). Scores handed out by score() are never changed in place: an edit makes a new
// one, so a view, an undo step or the audio plan can keep the one it has.
//
// ScoreBoxTarget is the target of a score box (ScoreSource, objects_score.cpp): the box's score is replaced after each
// edit (what plays follows at once), the patch gets an undo step when the score is kept in it (Edit > Undo in the
// patch takes the edit back, as for any box), and, for the roll's own window, a history of its own (Undo in the roll).
// It is meant to be replaced by the score document (ScoreDoc) when it lands: see the end of this file.
#pragma once

#include "moritz.h"

#include "music/score.h"
#include "music/score_transform.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace moritz {

namespace sc = musil::score;

class RollCore;

// ---- what a score box is to the views that edit it (objects_score.cpp)
class ScoreSource {
public:
    virtual ~ScoreSource() = default;
    virtual std::shared_ptr<const sc::Score> scoreNow() const = 0;   // never changed in place: a new one after a change
    virtual uint64_t scoreGeneration() const = 0;                   // changes with every new score
    virtual void setScore(std::shared_ptr<sc::Score> s) = 0;        // what plays changes at once
    virtual std::string scoreFile() const = 0;                      // the .score file it is kept in (resolved), or ""
    virtual bool saveScoreFile(std::string& err) = 0;               // into that file
    virtual std::string scoreTitle() const = 0;                     // "score piece"
    virtual Patch* scorePatch() const = 0;
    virtual bool scoreInPatch() const = 0;                         // false once removed (kept until the audio lets go)
    virtual RollCore* inlineRoll() { return nullptr; }             // the roll drawn inside the box (made when needed)
};
// the box is going (its destructor, maybe during an undo that replaces it): its targets let go of it, and follow the
// box that takes its place, if one does (settled from the event loop)
void scoreSourceClosed(ScoreSource* s, Patch* p, const std::string& id);

// What the application does for the roll's edits (set by App::init; unset in the engine tests and --render, where
// nothing more is done): an undo step in the document showing a patch, the patch marked edited, and where a box is,
// as a function that finds the box in its place later (after an undo or the code view replaced it), or nothing.
struct RollHost {
    std::function<void(Patch*)> pushUndo, markEdited;
    std::function<std::function<Box*()>(Patch*, const std::string& id)> locate;
};
RollHost& rollHost();

using ScorePath = std::vector<uint64_t>;     // the ids of the score events from the top down to the score edited
using Ids = std::vector<uint64_t>;

// a transformation of the selection (score_transform.h), as the roll's buttons and the box's messages ask for it
struct ScoreTransform {
    enum Kind { Transpose, Invert, Retrograde, Augment, Quantize, Humanize, Thin, Density, Shift } kind = Transpose;
    double a = 0, b = 0;            // Transpose: semitones; Invert: the axis (NaN: the middle of the selection's pitches);
                                    // Augment: the factor; Quantize: the grid (s), b the strength; Humanize: seconds, b dB;
                                    // Thin: keep (0..1); Shift: seconds
    std::vector<double> curve;      // Density
    uint32_t seed = 1;              // Humanize, Thin, Density
};
// applies one to a score (the ids of the score; none: all); false when it changes nothing; throws sc::Error
bool applyTransform(sc::Score& s, const Ids& ids, const ScoreTransform& t);

class RollEditTarget {
public:
    virtual ~RollEditTarget() = default;
    // ---- reading
    virtual std::shared_ptr<const sc::Score> score() const = 0;     // the top score as it is now
    virtual uint64_t generation() const = 0;                         // changes after every change, from anywhere
    virtual bool editable() const = 0;
    virtual std::string title() const = 0;
    virtual Patch* patch() const = 0;                                // where it plays (the transport), or nullptr
    virtual bool dirty() const { return false; }                     // edits not yet in its file
    virtual bool save(std::string& err) { err = "nothing to save"; return false; }
    // ---- edits: one undo step each, on the score at path (empty: the top); false when nothing changed (lastError
    // says why, if something went wrong)
    bool moveEvents(const ScorePath& path, const Ids& ids, double dt, double dpitch);
    bool resizeEvents(const ScorePath& path, const Ids& ids, double ddur);
    Ids copyEvents(const ScorePath& path, const Ids& ids, double dt, double dpitch);   // => the copies' ids
    uint64_t insertEvent(const ScorePath& path, const sc::Event& e);                  // => its id (0: none)
    bool removeEvents(const ScorePath& path, const Ids& ids);
    // a field of each event: at dur pitch cents dyn tech row gain az el label (a number or a word), or any other key
    // (kept with the event: Datum)
    bool setField(const ScorePath& path, const Ids& ids, const std::string& key, const sc::Datum& value);
    bool transform(const ScorePath& path, const Ids& ids, const ScoreTransform& t);
    bool replaceScore(std::shared_ptr<sc::Score> s, const std::string& label);       // the text view: all of it
    // ---- history
    virtual bool undo() = 0;
    virtual bool redo() = 0;
    virtual bool canUndo() const = 0;
    virtual bool canRedo() const = 0;
    std::string lastError;

protected:
    // the primitive: f changes a copy of the score at path and says whether it changed anything; the copy then takes
    // the place of the score (one undo step named label)
    virtual bool apply(const ScorePath& path, const std::string& label, const std::function<bool(sc::Score&)>& f) = 0;
    virtual bool replaceTop(std::shared_ptr<sc::Score> s, const std::string& label) = 0;
};

// The target of a score box (see the top). Bound to the box until it goes; then to the box that took its place
// (an undo in the patch, the code view), or to nothing (the score it had stays shown, read-only).
class ScoreBoxTarget : public RollEditTarget {
public:
    // localHistory: Undo and Redo of its own (the roll's window); without, only the patch's undo steps (inline)
    ScoreBoxTarget(ScoreSource* src, bool localHistory);
    ~ScoreBoxTarget() override;
    std::shared_ptr<const sc::Score> score() const override;
    uint64_t generation() const override;
    bool editable() const override { return src_ && src_->scoreInPatch(); }
    std::string title() const override;
    Patch* patch() const override { return src_ ? src_->scorePatch() : nullptr; }
    bool dirty() const override { return fileDirty_; }
    bool save(std::string& err) override;
    bool undo() override;
    bool redo() override;
    bool canUndo() const override { return src_ && !undo_.empty(); }
    bool canRedo() const override { return src_ && !redo_.empty(); }
    ScoreSource* source() const { return src_; }
    void unbind();                       // the box is gone
    void rebind(ScoreSource* s);         // the box that took its place
    // every target alive (the registry scoreSourceClosed walks)
    static std::vector<ScoreBoxTarget*>& all();

protected:
    bool apply(const ScorePath& path, const std::string& label, const std::function<bool(sc::Score&)>& f) override;
    bool replaceTop(std::shared_ptr<sc::Score> s, const std::string& label) override;

private:
    struct Step {
        std::shared_ptr<const sc::Score> score;
        std::string label;
    };
    void commit(std::shared_ptr<sc::Score> next, const std::string& label);
    void put(std::shared_ptr<const sc::Score> s);     // into the box, marked as an edit
    ScoreSource* src_;
    bool local_;
    bool fileDirty_ = false;
    std::shared_ptr<const sc::Score> last_;           // what it showed when the box went
    uint64_t gone_ = 0;                               // generations after the box went
    std::vector<Step> undo_, redo_;
};

// Switching to ScoreDoc (src/score_doc.h) when it lands: a ScoreDocTarget : RollEditTarget whose score() and
// generation() are the document's, whose apply() and replaceTop() are the document's edit (one undo step in its
// history, its listeners told) and whose undo/redo/canUndo/canRedo/dirty/save are the document's; then the roll's
// window and the score box make one (openRoll and the box's inline roll call makeRollTarget), and ScoreBoxTarget goes.
std::shared_ptr<RollEditTarget> makeRollTarget(ScoreSource* src, bool localHistory);

}  // namespace moritz
