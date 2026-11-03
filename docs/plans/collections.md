<!-- The agreed plan for three new kinds of collection: Queue, ROB, and Map. -->

# Plan: more kinds of collection

Today's collection is a set that keeps its members in join order. Hardware structures follow stricter disciplines, so
three new kinds join it. Each kind's checks are the ones its operations imply, and nothing more.

The work is done in the Chisel frontend and the shared runtime first. `oet` follows once this is finished.

## The kinds

| Kind | Declare | Join | Leave | Find |
|---|---|---|---|---|
| Set | `EventTrace.collection(new T)` | `add(i)` | `remove(i)` | views, choosers, `iter`, `size` |
| Queue | `EventTrace.queue(new T, entries)`, with `entries` optional | `add(i)`, at the tail | `pop()`; `clear { i => ... }` | as a Set |
| ROB | `EventTrace.rob(new T, entries, keys)` | `add(key, i)` | `pop()`; `rollback(newTail) { i => ... }`; `clear { i => ... }` | as a Set, and `rob(key)` |
| Map | `EventTrace.map(new T, entries)` | `add(key, i)` | `remove(i)` | as a Set, in join order, and `map(key)` |

The Set is today's collection, unchanged.

A **Queue** is a FIFO:

- `pop()` returns the head, and the one event declared on it carries its removal. `pop(k)` returns member `k`, for
  the `k`-th of a cycle's pops. The pops of one cycle must take the first members, as many as there are pops.
- `clear` removes every member. Its block must hold an event on its member, and that event carries the removal.
- When `entries` is declared, a queue holding more members than that at the end of a cycle is a violation.

A **ROB** is a queue whose tail allocates keys, as a reorder buffer's tail allocates tags:

- `keys` is the size of the key space: keys count modulo `keys`. vroom's scalar ROB has 16 entries and a 5-bit tag
  with its wrap bit, so `keys = 32`. A ROB without a wrap bit, such as doomv2's, has `keys` equal to `entries`.
  Elaboration checks that `entries` is at most `keys`.
- `add(key, i)` checks that `key`, the hardware's tag, is the key the ROB allocates next. The first `add` sets where
  allocation starts. A mismatch is a violation, since the model and the hardware disagree about the ROB.
- Several adds in one cycle allocate in site order.
- `rob(key)` picks the live member with that key. It is a violation if there is none.
- `rollback(newTail)` moves the tail back to `newTail`, the value the hardware writes into its tail pointer: the
  members from `newTail` through the youngest leave, and allocation continues from `newTail`. `newTail` must be a
  live key or the current tail, which removes nothing. Any other key is a violation. The block must hold an event on
  its member, and that event carries the removal.
- `pop()` and `clear` work as on a queue.

A **Map** holds members under keys that the hardware chooses, as a free list does:

- `add(key, i)` checks that `key` is below `entries` and free once the cycle's removals apply.
- `map(key)` picks the member under that key. It is a violation if there is none.
- `remove(i)` removes any member.
- Views run in join order, as for a Set.

`pop`, `rollback`, and `clear` are, in effect, `iter` with `remove(i)` over the members the operation chooses. They
stay distinct operations in the runtime, so that it can check what each one implies.

## Not supported

A reset in the middle of a run does not restart a ROB's allocation, so the next `add` is a violation.

## Later

- Reading a member's key in a predicate, or capturing it as an event field.
- Recording which slot each member occupies in the trace, for bart's view.
- A bit test in predicates, for structures that kill entries by branch mask.

## Tests

- `examples/structures`: a small pipeline with a fetch queue, a ROB with completions out of order and squashes, and a
  reservation-station map. Its `check.awk` checks the records against the design's behavior.
- A misuse case for each new runtime check, each of which must stop the run with its message.
- An elaboration-error case for each new declaration check.
