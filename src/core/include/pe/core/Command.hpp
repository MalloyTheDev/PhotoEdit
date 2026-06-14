#pragma once

#include "pe/core/DocumentChange.hpp"

#include <string>

namespace pe {

class Document;

// The universal unit of mutation. Undo/redo, the History panel, actions, batch,
// and scripting are all sequences of commands (ADR-0005). execute/undo mutate the
// document through its command-facing API and return a DocumentChange describing
// what changed; History notifies observers and updates the dirty flag.
//
// serialize() (for recorded actions/scripting) arrives with automation in M10.
class Command {
public:
    virtual ~Command() = default;

    [[nodiscard]] virtual std::string name() const = 0;
    virtual DocumentChange execute(Document&) = 0;
    virtual DocumentChange undo(Document&) = 0;
};

}  // namespace pe
