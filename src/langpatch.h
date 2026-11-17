// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// langpatch.h — patches in the Moritz language (langpatch.cpp).
//
// (patch ...), (obj ...) and (connect ...) are forms of the language, so a patch file is a program and a
// program can write patches. Arguments are taken as written, as in every patch file:
//
//     (obj osc cycle~ 440 (pos 20 70))
//
// and a comma computes one: ,x is the value of x, ,(...) the value of a form (a list is spliced):
//
//     each (range 8) (function (k) {
//         obj ,(sym "osc" k) cycle~ ,(* 110 (+ k 1)) (pos ,(+ 20 (* k 90)) 120)
//     })
//
// (patch ...) gives a patch: a value holding the text of the patch, as a file holds it.
#pragma once

#include "lang/core.h"

#include <string>

namespace moritz {

// the forms and the functions on patch values, into an interpreter
void addPatchLanguage(lang::Interp& I);

// a patch value and its text
lang::vptr makePatchValue(const std::string& text);
bool patchValueText(const lang::vptr& v, std::string& text);

// a (patch ...) file that holds code (a form other than obj and connect, or a comma): only the language
// can read it. A file whose first form is not (patch ...) at all is code (codewindow.h, isCodeText).
bool patchNeedsLanguage(const std::string& text);

// the file has a (patch ...) at its top level (alone, or in a top-level line such as var p (patch ...)):
// running it makes a patch, so it opens as one
bool makesPatch(const std::string& text);

// the boxes of a patch text, as records ((id x) (class x) (args (...)) (x n) (y n))
lang::vptr patchBoxes(const std::string& text);

}  // namespace moritz
