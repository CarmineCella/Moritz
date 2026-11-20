// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// datasets.h — the sample databases by name, as Moritz uses them: the same place as Musil's (musil/src/music/datasets.h:
// ~/.musil/datasets.mu, ~/Documents/Moritz/datasets, the bundled MicroSOL), the Datasets window (Extras > Datasets...,
// and the Datasets section of the Settings), and the background work it starts: TinySOL downloaded, a zip installed, a
// folder of sounds analysed into a feature file (Musil's db_generate, the analysis of Orchidea's dbgen) and registered.
#pragma once

#include "jobs.h"
#include "musil_host.h"

#include <functional>
#include <mutex>
#include <string>

namespace moritz {

// where Moritz looks: the shared configuration and default folder, and the MicroSOL among the program's data
musil::datasets::Place datasetsPlace();
// the feature file of a dataset, by name (throws std::runtime_error with what was looked at); any thread
std::string resolveDataset(const std::string& name);
// where the analysis of a folder of sounds is written: next to the folder, <name>.spectrum.db (the *SOL layout, which
// every loader reads: the sounds are found in the folder next to the feature file, without copying them)
std::string analysisPath(const std::string& folder, const std::string& name);

// A dataset downloaded, installed or analysed as a background job (Job: progress, cancel, the result on the GUI thread).
// What is made is registered by name when it is not found by name already, so that orchestra <name> finds it.
class DatasetTask {
public:
    std::function<void(float, const std::string&)> onProgress;                  // GUI thread: 0..1, what is being done
    std::function<void(const std::string& file, const std::string& error)> onDone;   // GUI thread; not after cancel()
    // each returns false (and says why in onDone) when it cannot start
    bool fetch(const std::string& name);
    bool install(const std::string& name, const std::string& archive);
    bool analyse(const std::string& folder, const std::string& name, const musil::db_gen_params& params, bool replace);
    void cancel() { job_.cancel(); }
    bool running() const { return job_.running(); }
    std::string stage() const;

private:
    void start(std::function<std::string(Job&)> work);
    void setStage(const std::string& s);
    Job job_;
    mutable std::mutex stageMutex_;
    std::string stage_;
};

void showDatasets();                       // Extras > Datasets...
std::string datasetsFolderText();          // the folder in use, for the Settings
void chooseDatasetsFolder(bool useDefault);   // the Settings' Choose... and Default
bool datasetsTestAction(const std::string& verb, const std::string& arg);   // guitest.cpp: act=datasets...

}  // namespace moritz
