#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace agent {

// Stage 5b: Fact-forest metadata describing how a MemoryEntry fits
// into the hierarchical tree of facts. Stages 5b / 6 / 8 read these
// fields; older stages ignore them. Kept separate from MemoryState.h
// so the existing replay/reducer path doesn't need to know about the
// forest concept.

enum class FactLevel {
    Atomic,        // L0: a single grounded fact
    Composite,     // L1: multiple atomic facts merged
};

enum class FactConfidence {
    Grounded,      // has source evidence in the transcript
    Deduced,       // deterministically derivable from child facts
};

}  // namespace agent
