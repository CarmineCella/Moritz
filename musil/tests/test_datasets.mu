; test_datasets: the sample databases found by name (music/datasets.h): the order (registered, the folder chosen, the
; default folder, the bundled MicroSOL), the configuration file written and read back, a malformed one, bad inputs.
; Nothing is downloaded: the installation from an archive is tested from C++ (engines.cpp), on a zip made there.
load "test.mu"
load "music.mu"

var dir "/tmp/musil_test_datasets"
mkdir (concat dir "/chosen/Sub")
mkdir (concat dir "/elsewhere")
; this interpreter's own configuration file, not the user's
datasets-config (concat dir "/datasets.mu")
if (exists? (datasets-config)) { remove (datasets-config) }
each (list "/chosen/TestSOL2021.spectrum.db" "/chosen/Sub/OtherSOL.spectrum.db" "/elsewhere/mine.spectrum.db" "/chosen/notes.txt")
     (function (f) (if (exists? (concat dir f)) (remove (concat dir f)) nil))
check (ends-with? (datasets-config) "musil_test_datasets/datasets.mu") "datasets-config: this interpreter's file"

; --- the bundled MicroSOL, and a name that is nowhere ---
check (dataset? "microsol") "dataset?: the bundled MicroSOL"
check (ends-with? (dataset "microsol") "microsol/microsol.spectrum.db") "dataset: MicroSOL's feature file"
check (ends-with? (dataset 'MicroSOL) "microsol.spectrum.db") "dataset: a symbol, in any case"
check (not (dataset? "testsol")) "dataset?: a name that is nowhere"
var missing (try (dataset "testsol") catch e e)
check (contains? missing "no dataset named \"testsol\"") "dataset: a missing name is a clear error"
check (contains? missing "dataset-register!") "dataset: the error says how to get it"
check (contains? (try (dataset "tinysol") catch e e) "dataset-fetch") "dataset: for tinysol, the error says it can be downloaded"
check (equal? (get (last (datasets)) 'where) "bundled") "datasets: the bundled one last"

; --- the folder chosen: at its top and one folder down, by the short name and the full one ---
write (concat dir "/chosen/TestSOL2021.spectrum.db") "spectrum 2048 256 2\n/Vn/Vn-ord-C4-mf.wav;1;2\n/Vc/Vc-ord-C3-mf.wav;3;4\n"
write (concat dir "/chosen/Sub/OtherSOL.spectrum.db") "spectrum 2048 256 2\n/Ob/Ob-ord-C4-mf.wav;1;2\n"
write (concat dir "/chosen/notes.txt") "not a dataset"
datasets-folder! (concat dir "/chosen")
check (ends-with? (datasets-folder) "musil_test_datasets/chosen") "datasets-folder!: the folder chosen"
check (ends-with? (dataset "testsol") "chosen/TestSOL2021.spectrum.db") "dataset: a feature file of the folder, by its name without the year"
check (ends-with? (dataset "TestSOL2021") "chosen/TestSOL2021.spectrum.db") "dataset: and by its full name"
check (ends-with? (dataset "othersol") "Sub/OtherSOL.spectrum.db") "dataset: one folder down"
check (equal? (get (head (datasets)) 'where) "folder") "datasets: the folder's, where they come from"

; --- a registered path comes first ---
write (concat dir "/elsewhere/mine.spectrum.db") "spectrum 2048 256 2\n/Fl/Fl-ord-C5-pp.wav;5;6\n"
check (ends-with? (dataset-register! "TestSOL" (concat dir "/elsewhere/mine.spectrum.db")) "elsewhere/mine.spectrum.db") "dataset-register!: the path, absolute"
check (ends-with? (dataset "testsol") "elsewhere/mine.spectrum.db") "dataset: a registered path before the folder's"
check (ends-with? (dataset "mine") "elsewhere/mine.spectrum.db") "dataset: a registered file by its own name too"
var first (head (datasets))
check (and (equal? (get first 'name) "testsol") (equal? (get first 'where) "registered") (== (get first 'present) 1)) "datasets: the registered one first"
; the configuration written and read back: what another interpreter (or Moritz) reads
var text (read (datasets-config))
check (contains? text "dataset testsol \"") "the configuration: the registration, in Musil's syntax"
check (contains? text "folder \"") "the configuration: the folder"
check (== (length (filter (split text "\n") (function (l) (starts-with? l "dataset ")))) 1) "the configuration: a name registered once"
dataset-register! "testsol" (concat dir "/elsewhere/mine.spectrum.db")
check (== (length (filter (split (read (datasets-config)) "\n") (function (l) (starts-with? l "dataset ")))) 1) "dataset-register!: again, replaced"
check (not (any? (ls dir) (function (f) (contains? f ".tmp-")))) "the configuration: written through a temporary file, none left"

; --- db-load and db-load-first by name ---
check (== (db-size (db-load "testsol")) 1) "db-load: a name"
check (== (db-size (db-load-first (list "nosuchsol" "testsol" "microsol"))) 1) "db-load-first: the first name that is there"
check (== (db-size (db-load-first (list "fullsol-not-here" "microsol"))) 32) "db-load-first: MicroSOL last"
check (contains? (try (db-load-first (list "nosuchsol")) catch e e) "none of") "db-load-first: nothing there, an error"
check (not (dataset-name? "x/y")) "dataset-name?: a path is not a name"
check (not (dataset-name? "TinySOL.spectrum.db")) "dataset-name?: a feature file is not a name"
check (dataset-name? "fullsol") "dataset-name?: a name"

; --- a registered file gone: the folder's is found, and the list says it is gone ---
remove (concat dir "/elsewhere/mine.spectrum.db")
check (ends-with? (dataset "testsol") "chosen/TestSOL2021.spectrum.db") "dataset: a registered file gone, the folder's"
check (== (get (head (datasets)) 'present) 0) "datasets: a registered file gone is not present"
check (dataset-forget! "testsol") "dataset-forget!: the registration removed"
check (not (dataset-forget! "testsol")) "dataset-forget!: nothing left to remove"
check (not (contains? (read (datasets-config)) "dataset testsol")) "dataset-forget!: written"

; --- bad inputs ---
check (contains? (try (dataset-register! "x" (concat dir "/chosen/notes.txt")) catch e e) "not a feature file") "dataset-register!: not a feature file"
check (contains? (try (dataset-register! "a b" (concat dir "/chosen/TestSOL2021.spectrum.db")) catch e e) "not a dataset's name") "dataset-register!: not a name"
check (contains? (try (dataset-register! "x" (concat dir "/nothing.spectrum.db")) catch e e) "not on disk") "dataset-register!: no such file"
check (contains? (try (dataset 42) catch e e) "expected a dataset's name") "dataset: not a name"
check (contains? (try (dataset-fetch "nosuchsol") catch e e) "nothing to download") "dataset-fetch: only what can be downloaded"
check (contains? (try (dataset-install "x" (concat dir "/nothing.zip")) catch e e) "nothing.zip") "dataset-install: no such archive"

; --- a malformed configuration: an error that names the file and the line, never a crash ---
write (datasets-config) "folder \"/tmp\"\nnonsense 1 2\n"
var bad (try (dataset "microsol") catch e e)
check (and (contains? bad "datasets.mu:2") (contains? bad "unknown setting nonsense")) "a malformed configuration: the line and what is wrong"
write (datasets-config) "folder (\n"
check (contains? (try (datasets) catch e e) "is not readable") "a configuration that does not parse"
write (datasets-config) "dataset fullsol\n"
check (contains? (try (dataset "fullsol") catch e e) "dataset takes a name and a path") "a registration without its path"
write (datasets-config) "dataset \"../x\" \"/a.db\"\n"
check (contains? (try (dataset "x") catch e e) "not a dataset's name") "a registration with a strange name"
write (datasets-config) "folder \"a \\\"quoted\\\" \\\\ name\"\n"
check (ends-with? (datasets-folder) "a \"quoted\" \\ name") "the configuration: quotes and backslashes read back"
; (on Windows a backslash in a folder's path is a separator: the folder comes back with a slash)
datasets-folder! (concat dir "/we \"quote\" \\ it")
check (ends-with? (datasets-folder) (if (equal? (platform) "windows") "we \"quote\" / it" "we \"quote\" \\ it")) "the configuration: quotes and backslashes written and read back"
datasets-folder! nil
check (not (ends-with? (datasets-folder) "it")) "datasets-folder! nil: the default again"

remove (datasets-config)
each (list "/chosen/TestSOL2021.spectrum.db" "/chosen/Sub/OtherSOL.spectrum.db" "/chosen/notes.txt") (function (f) (remove (concat dir f)))
report "test_datasets"
