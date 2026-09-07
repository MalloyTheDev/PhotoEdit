#pragma once

#include "pe/core/DocumentChange.hpp"

#include <cstdint>
#include <string>

namespace pe {

class Document;

// The universal unit of mutation. Undo/redo, the History panel, actions, batch,
// and scripting are all sequences of commands (ADR-0005). execute/undo mutate the
// document through its command-facing API and return a DocumentChange describing
// what changed; History notifies observers and updates the dirty flag.
//
// serialize() (for recorded actions/scripting) arrives with automation in M10.
//
// Exception contract (the BASIC guarantee, not the strong one):
//
//   - execute() and undo() may throw. Several commands allocate after they have begun
//     mutating (GroupLayersCommand builds a vector after it starts removing layers;
//     AddLayerMaskCommand allocates a Mask and does a canvas-wide fill; CropCommand
//     allocates a move command per layer), so a bad_alloc mid-mutation is a real path in
//     a codebase that runs near its memory budget by design. Requiring the strong
//     guarantee would mean making every command transactional, which is not worth it.
//
//   - A throw must leave the document VALID, not necessarily unchanged. Partial mutation
//     is permitted.
//
//   - undo() must therefore tolerate being called after an execute() that threw partway,
//     and reverse exactly as much as that execute() applied. History keeps a throwing
//     command on the undo stack precisely so this is possible: the command is the only
//     object that knows what it managed to do. A command that cannot honour this must
//     instead unwind internally and rethrow, so that its execute() is all-or-nothing.
//
//   - History guarantees for its part that a throwing command is never dropped, that no
//     stack operation around the call can throw (destinations are reserved first), and
//     that observers are notified conservatively so nothing keeps showing stale pixels.
class Command {
public:
    virtual ~Command() = default;

    [[nodiscard]] virtual std::string name() const = 0;
    virtual DocumentChange execute(Document&) = 0;
    virtual DocumentChange undo(Document&) = 0;

    // Approximate bytes this command keeps alive while it sits on an undo stack, so
    // History can bound memory by SIZE and not only by step count. A step count alone is
    // how a painting session exhausts memory: one 300 px brush stroke over a long drag
    // retains tens of megabytes, and the default hundred-step limit does not engage until
    // far past what the machine has.
    //
    // Approximate on purpose. Tile deltas share storage copy-on-write with neighbouring
    // versions, so an exact figure would mean walking the whole stack to find what is
    // shared. Over-counting is the safe direction: it trims sooner.
    //
    // The default of zero is correct for a command whose payload is a handful of scalars
    // (an opacity, a blend mode, a layer id). Any command that holds pixels, a mask, a
    // selection, or a detached layer MUST override this, and a test asserts that the ones
    // known to do so report a non-trivial figure.
    [[nodiscard]] virtual std::int64_t retainedBytes() const noexcept { return 0; }
};

}  // namespace pe
