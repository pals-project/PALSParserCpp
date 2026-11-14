#ifndef PALS_PROBLEM_H
#define PALS_PROBLEM_H

// The internal form of a problem found while reading or expanding a document,
// and the one funnel every part of the library reports through. Converted to
// the C `struct problem_list` at the end of parse_and_expand_PALS.
//
// Kept in its own header because every stage of the expansion pipeline
// (pals_pipeline.h) and the name checks in pals_check.cpp report into the same
// list. Not part of the public C API declared in PALSParserCpp.h.

#include <string>
#include <vector>

#include "PALSParserCpp.h"

namespace pals {

// Mirrors the C enums of the same names; see PALSParserCpp.h for what the
// values mean. Kept as a scoped enum internally so an unannotated call site
// cannot silently pass the wrong one -- the two are both small ints, and the
// argument order would not protect us.
enum class Severity { Error, Warning };
enum class Origin { Input, Unsupported, Unspecified };

struct Problem {
    std::string message;
    std::string path;  // Shallow "group.param"; empty when not tied to a node.
    Severity severity = Severity::Error;
    Origin origin = Origin::Input;
};

using ProblemList = std::vector<Problem>;

// Append a problem, skipping exact duplicates. Expansion copies a definition
// into every use, so the same underlying issue can be reached many times; the
// shallow paths used at the call sites keep those copies collapsing to one
// entry.
//
// The defaults say "the document is wrong and the output cannot be trusted",
// which is what the great majority of problems are. Only the handful that are
// this library's limitation, or the standard's silence, name their kind -- so a
// call site that says nothing is making the common claim, not skipping the
// question.
inline void add_problem(ProblemList& problems, const std::string& msg,
                        const std::string& path = "",
                        Severity severity = Severity::Error,
                        Origin origin = Origin::Input) {
    for (const Problem& p : problems)
        if (p.message == msg) return;
    problems.push_back(Problem{msg, path, severity, origin});
}

}  // namespace pals

#endif  // PALS_PROBLEM_H
