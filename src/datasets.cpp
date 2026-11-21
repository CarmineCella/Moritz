// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// datasets.cpp — where Moritz finds datasets, and the background work of the Datasets window (see datasets.h)
#include "datasets.h"
#include "gui.h"

#include <filesystem>

namespace moritz {

namespace ds = musil::datasets;

ds::Place datasetsPlace() {
    ds::Place p;
    if (std::string d = resourceDir("data"); !d.empty()) {
        std::error_code ec;
        std::string f = d + "/microsol/microsol.spectrum.db";
        if (std::filesystem::is_regular_file(f, ec)) p.bundled.push_back(f);
    }
    return p;
}

std::string resolveDataset(const std::string& name) { return ds::resolve(datasetsPlace(), name); }

std::string analysisPath(const std::string& folder, const std::string& name) {
    std::filesystem::path f = std::filesystem::path(folder).lexically_normal();
    if (f.filename().empty()) f = f.parent_path();          // "x/" names x
    return (f.parent_path() / (name + ".spectrum.db")).generic_string();
}

std::string DatasetTask::stage() const {
    std::lock_guard<std::mutex> g(stageMutex_);
    return stage_;
}
void DatasetTask::setStage(const std::string& s) {
    std::lock_guard<std::mutex> g(stageMutex_);
    stage_ = s;
}

void DatasetTask::start(std::function<std::string(Job&)> work) {
    auto file = std::make_shared<std::string>(), err = std::make_shared<std::string>();
    setStage("starting");
    job_.onProgress = [this](float p) { if (onProgress) onProgress(p, stage()); };
    job_.start(
        [work = std::move(work), file, err](Job& job) {
            try { *file = work(job); }
            catch (const ds::Cancelled&) { *err = "cancelled"; }
            catch (const std::exception& e) { *err = e.what(); }
        },
        [this, file, err] {
            job_.cancel();          // the job has ended: its thread joined now (the window lives as long as the program)
            setStage(err->empty() ? "done" : "failed");
            if (onDone) onDone(*file, *err);
        });
}

namespace {
// the job's progress and cancel, as Musil's progress callback
ds::Progress progressOf(Job& job, const std::function<void(const std::string&)>& stage) {
    return [&job, stage](double f, const std::string& what) {
        stage(what);
        job.progress((float)f);
        return !job.cancelled();
    };
}
}  // namespace

bool DatasetTask::fetch(const std::string& name) {
    const ds::Source* s = ds::source(name);
    std::string folder;
    try { folder = ds::folder_in_use(datasetsPlace(), ds::read_config(datasetsPlace().config)); }
    catch (const std::exception& e) { if (onDone) onDone("", e.what()); return false; }
    if (!s || folder.empty() || running()) {
        if (onDone) onDone("", !s ? "nothing to download for " + name : folder.empty() ? "no datasets folder" : "already working");
        return false;
    }
    start([this, s, folder](Job& job) { return ds::fetch(*s, folder, progressOf(job, [this](const std::string& w) { setStage(w); })); });
    return true;
}

bool DatasetTask::install(const std::string& name, const std::string& archive) {
    const ds::Source* s = ds::source(name);
    std::string folder;
    try { folder = ds::folder_in_use(datasetsPlace(), ds::read_config(datasetsPlace().config)); }
    catch (const std::exception& e) { if (onDone) onDone("", e.what()); return false; }
    if (folder.empty() || running()) { if (onDone) onDone("", folder.empty() ? "no datasets folder" : "already working"); return false; }
    std::string sha = s ? s->sha256 : "", stem = s ? s->stem : name;
    start([this, archive, folder, sha, stem](Job& job) {
        return ds::install(archive, folder, sha, stem, progressOf(job, [this](const std::string& w) { setStage(w); }));
    });
    return true;
}

bool DatasetTask::analyse(const std::string& folder, const std::string& name, const musil::db_gen_params& params, bool replace) {
    std::string why;
    std::error_code ec;
    if (running()) why = "already working";
    else if (!ds::valid_name(ds::name_key(name))) why = "not a dataset's name: \"" + name + "\" (letters, digits, - _ .)";
    else if (!std::filesystem::is_directory(folder, ec)) why = "not a folder: " + folder;
    else {
        try { musil::db_gen_check(params); } catch (const std::exception& e) { why = e.what(); }
    }
    std::string out = analysisPath(folder, name);
    if (why.empty() && !replace && std::filesystem::exists(out, ec)) why = out + " is already there";
    if (!why.empty()) { if (onDone) onDone("", why); return false; }
    std::string config = datasetsPlace().config, key = ds::name_key(name);
    start([this, folder, out, params, config, key](Job& job) {
        setStage("analysing");
        size_t n = musil::db_generate(folder, out, params, [&job](size_t k, size_t total) {
            job.progress(total ? .98f * (float)k / (float)total : 0.f);
            return !job.cancelled();
        });
        if (n == 0) {
            std::error_code e;
            std::filesystem::remove(out, e);
            throw std::runtime_error("no WAV file under " + folder);
        }
        setStage("registering");
        ds::register_dataset(config, key, out);
        job.progress(1.f);
        return out;
    });
    return true;
}

}  // namespace moritz
