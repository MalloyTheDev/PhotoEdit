# Golden reference images

Committed reference output for the subsystems that draw, as
[docs/04-coding-standards.md](../../docs/04-coding-standards.md) requires. The harness is
[`tests/golden.hpp`](../golden.hpp); the cases are in
[`tests/core/test_golden.cpp`](../core/test_golden.cpp).

## Regenerating

```bash
PE_GOLDEN_UPDATE=1 ./build/mingw/bin/pe_core_tests
```

Every case rewrites its reference in place, so the change arrives as a reviewable diff.
**Look at the images before committing them.** A regenerated golden is a claim that the new
output is correct, and a reference captured from a bug turns that bug into the
specification.

## When a golden fails

The failure names the worst single-channel difference and the mean, and writes
`<name>.actual.png` next to the reference so the two can be opened side by side. Those
`.actual.png` files are build output, not fixtures, and are gitignored.

## Tolerances

`kExact` for output defined bit for bit. `kKernelRewrite` (one LSB per channel, mean under
0.25) for a kernel that may legitimately be reimplemented with different rounding, such as
a running-sum box blur replacing the naive one. Two numbers rather than one: a per-pixel
bound alone would either block that rewrite or have to be loosened until real regressions
slipped through.

## Why every case also asserts something structural

A golden test on its own cannot tell a correct reference from a wrong one. Each case
therefore also asserts a property the reference plays no part in (a blur reduces local
contrast, a median leaves a hard edge intact, a gradient is monotonic along its axis).
That half is also the only half that runs in the no-optional-deps CI lane, which has no
PNG codec.
