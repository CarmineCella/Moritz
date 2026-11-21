// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// roll_edit.h — what the roll asks of the score it edits: edit intents (move, resize, copy, insert, remove, set a
// field, detune, transform, replace the whole score), each one undo step, on the score at a path of sub-scores.
//
// The roll (roll.h) never changes a score itself: it names what the user did, and a RollEditTarget does it. The
// intents are written on one primitive, apply(): a function that changes a copy of the score at a path, which then
// becomes the score (one undo step named after the intent).
//
// ScoreDocTarget is the one target there is: the roll edits a ScoreDoc (score_doc.h), the document the timeline edits
// too, so the timeline and the roll of a score are views of one document: one undo history, one selection, an edit
// in one shown in the other at once, saved the same way. apply() makes the copy, runs the intent on it and hands the
// document the difference (events removed, inserted, replaced, the rows, the header) as one Edit: the document keeps
// only what changed, plays it at once (the box it is bound to), marks the patch edited when the score is kept in it,
// and tells every view. A score placed inside another (a path) is the document of that placement
// (ScoreDoc::ofPlacement: an instance, every placement following), with its own history, as in the timeline. Scores
// handed out by score() are copies made once per change: a view can keep the one it has while the document changes.
#pragma once

#include "moritz.h"

#include "music/score.h"
#include "music/score_transform.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace moritz {

namespace sc = musil::score;

class ScoreDoc;

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
    // where the view is (the score at path): undo, redo, dirty and save act on that score
    virtual void focus(const ScorePath&) {}
    // the selection shared with the other views of the score at path (false: none is shared)
    virtual bool sharedSelection(const ScorePath&, Ids&) const { return false; }
    virtual void shareSelection(const ScorePath&, const Ids&) {}
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
    bool detune(const ScorePath& path, const Ids& ids, double dcents);    // the notes' cents by that much
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

// The roll's edits on a score document (see the top). The target keeps the documents of the scores placed inside
// that it was taken into (their unsaved edits are not lost while it lives).
class ScoreDocTarget : public RollEditTarget {
public:
    explicit ScoreDocTarget(std::shared_ptr<ScoreDoc> doc);
    ~ScoreDocTarget() override;
    std::shared_ptr<const sc::Score> score() const override;
    uint64_t generation() const override;
    bool editable() const override { return true; }
    std::string title() const override;
    Patch* patch() const override;
    bool dirty() const override;
    bool save(std::string& err) override;
    void focus(const ScorePath& path) override;
    bool sharedSelection(const ScorePath& path, Ids& out) const override;
    void shareSelection(const ScorePath& path, const Ids& ids) override;
    bool undo() override;
    bool redo() override;
    bool canUndo() const override;
    bool canRedo() const override;
    const std::shared_ptr<ScoreDoc>& document() const { return doc_; }     // the top
    std::shared_ptr<ScoreDoc> focused() const;                             // the score the view is in
    std::vector<std::shared_ptr<ScoreDoc>> documents() const;              // the top, then those placed inside
    // told when its document changes (ScoreDoc's Change flags: an edit inside a score placed in it is a change of it
    // too); one at a time
    void setListener(std::function<void(unsigned)> f);

protected:
    bool apply(const ScorePath& path, const std::string& label, const std::function<bool(sc::Score&)>& f) override;
    bool replaceTop(std::shared_ptr<sc::Score> s, const std::string& label) override;

private:
    std::shared_ptr<ScoreDoc> docAt(const ScorePath& path) const;   // made (and kept) on first use
    bool commitDiff(ScoreDoc& d, const sc::Score& next, const std::string& label);
    std::shared_ptr<ScoreDoc> doc_;
    mutable std::map<ScorePath, std::shared_ptr<ScoreDoc>> subs_;
    ScorePath focus_;
    mutable std::shared_ptr<const sc::Score> snap_;     // the copy handed out, and the version it is of
    mutable uint64_t snapVersion_ = ~0ull;
    std::function<void(unsigned)> listener_;
    int token_ = 0;
};

// the roll's target for a document (the roll's window and the score box's own roll each make one)
std::shared_ptr<ScoreDocTarget> makeRollTarget(std::shared_ptr<ScoreDoc> doc);

}  // namespace moritz
