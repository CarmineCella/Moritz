MicroSOL: a very small sample database in the layout of the *SOL databases (Ircam's TinySOL,
OrchideaSOL, FullSOL): the feature file `microsol.spectrum.db` next to a folder holding the
sounds by family and instrument (the paths in the feature file are relative to that folder;
the loader finds it by content, whatever it is called). Four instruments (Ob, Hn, Vn, Vc), eight sounds each around C4-G4, mf (one ff),
32 sounds in all, so that the music library and its tests run from a fresh clone.

The full sets are much bigger and are not in the repository. TinySOL (about 800 MB) is published
by Ircam; put it anywhere and load it with `(db-load "path/to/TinySOL.spectrum.db")`, or list it
first in a `db-load-first`, as the language's examples do, so that MicroSOL is used when it is
not there.
