# Contributing

Thanks for helping. This project is a replacement C64 KERNAL plus the TeensyROM+ firmware that serves it; a lot of it can only be proven on a real
C64, so the branches say how far a change has been proven.

## Branches

| Branch | What is on it |
|---|---|
| `main` | **Verified on real hardware** (a C64 with a TR+ running this firmware and KERNAL image) and passing `make test`. What a user should flash. |
| `bleeding-edge` | **Code complete and passing the local tests, but not yet verified on hardware.** Everything new lands here first. May contain things that do not work on a real machine yet. |

Flow:

1. Branch from `bleeding-edge` (or from `main` for a hotfix) and open your pull request against `bleeding-edge`.
2. When someone has tried the change on hardware and reported the result, promote it: a pull request from `bleeding-edge` (or from the verified
   commits) to `main`. Move the item in `CLAUDE.md` from "NOT on hardware" to the verified list in the same pull request.
3. Keep `bleeding-edge` merged with `main` (merge, do not rebase shared branches), so a hotfix on `main` does not diverge.

A pull request should say plainly what was and what was not run: host tests, VICE, real C64. Do not claim a hardware result you did not get.

## Tests are not optional

Every change comes with tests, in the same pull request:

* a bug fix gets a test that fails without the fix;
* a feature gets tests for its normal path, its limits and its error cases;
* firmware glue (`firmware/IOH_VDrive.c`) is exercised by `tests/fw_host`, the portable engine (`d64/`) by `tests/test_*.py`, the 6502 patches by the
  VICE runs (`tools/vdrive_mock.py`) and, where VICE is not available, a simulator or a model (see `tests/test_irq_border.py`, `tests/test_vdconfig.py`);
* a C64 program (`util/`, `tests/basic/`, `tests/asm/`) needs a way to check it on the host if at all possible, and a note in its README on how to check it on hardware.

Run `make test` before pushing (about 4 minutes; needs `cc65`, VICE with its ROMs, a C/C++ compiler, Python 3; `pip install py65` for the
6502 routine test). If you cannot run part of it (for example no ROM in a cloud environment), say which part in the pull request.

## Rules that have bitten before

* **No Commodore ROM bytes in the repository.** Only patches, built onto your own dump (`make build/kernal-tr.bin STOCK=...`). `build/` stays ignored.
* **`d64/` must stay portable C99 that also compiles as C++** (the firmware compiles it inside the `.ino`). `make` and `tests/fw_host/run.py` check this.
* **Bus-handler code must be short and use fast RAM.** The IO1/IO2 handlers answer the C64 within one cycle; never add work before the data lines are
  driven, and keep anything the bus ISR touches in `vio_hot_t`. See "Gotchas" in `CLAUDE.md`.
* **Anything not reachable from the bus ISR is `VD_COLD`** (flash).
* **The mailbox is not live during BASIC init**; do not call it from reset code.
* **Firmware version:** any change to firmware behaviour (`d64/`, `firmware/`, the patch edits) bumps `firmware/REV`, and you regenerate the patch with
  `tools/make_tr_patch.sh /path/to/pristine/TeensyROM` and check it with `git apply --check` on a clean upstream checkout.
* A hook that overwrites bytes at an entry point of the stock KERNAL must be checked against every fall-through into those bytes.
* Names are PETSCII inside the C code; the glue converts to and from ASCII for the host file system.

## Documentation

* `docs/DESIGN.md`: a numbered section for each feature (what, why, limits, how it was tested). Add yours.
* `README.md`: what a user needs to know. `CHANGELOG.md`: one entry per user-visible change under "Unreleased".
* `CLAUDE.md`: the status list (what is proven where), commands and gotchas; it is also read by coding assistants, so keep it current.

## Commits and pull requests

* Small, focused commits with a message that says why. Mention the firmware revision when it changes.
* Describe the change, how it was tested (host / VICE / hardware) and what a reviewer should try on a real C64.
* Report hardware results, good or bad, in the pull request or an issue: which TR+ firmware version, KERNAL image, drive (SD or USB) and what you did.
