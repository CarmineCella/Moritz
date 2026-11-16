// Copyright (c) 2026 Carmine-Emanuele Cella. SPDX-License-Identifier: BSD-2-Clause
// objects_maple.h — what the sound-type objects use of the Maple objects:
// a dictionary by name, and a book read from a buffer~.
#pragma once

#include "maple.h"
#include "objects.h"

#include <memory>
#include <string>

namespace moritz {

// the dictionary of the atoms object `name` (nullptr if there is none)
std::shared_ptr<const maple::Dictionary> findDictionary(Patch* p, const std::string& name);
// a book stored in a buffer~ by pursuit (false if it isn't one)
bool loadBook(const SampleBuffer& b, maple::Book& book);

}  // namespace moritz
