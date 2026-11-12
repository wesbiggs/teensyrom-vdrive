// SPDX-License-Identifier: MIT
// When an open disk image's writes go to the card, for any caller that writes sectors into an image file and syncs it
// itself (the TR block device; VDrive's units). A sync the card fails leaves the image dirty, so it is tried again
// after the next idle period rather than reported as done; only the first failure in a row is worth a message.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// A quarter second after the last write: GEOS writes a block at a time and never syncs, and a sync per block would
// stall the main loop for the card's write time on every one (docs/TR_Block_Device.md, "Writes reach the card later")
#define IMAGE_WRITEBACK_IDLE_MS 250

struct ImageWriteBack
{
   uint32_t LastWritemS;
   bool Dirty, FailureReported;
};

static inline void ImageWriteBackWrote(struct ImageWriteBack *Back, uint32_t Now)
{
   Back->Dirty = true;
   Back->LastWritemS = Now;
}

static inline bool ImageWriteBackDue(const struct ImageWriteBack *Back, uint32_t Now)
{
   return Back->Dirty && Now - Back->LastWritemS > IMAGE_WRITEBACK_IDLE_MS;
}

// Records how a sync went. Returns true for the failure that should be reported: the first since the last success.
static inline bool ImageWriteBackSynced(struct ImageWriteBack *Back, bool Reached, uint32_t Now)
{
   if (Reached)
   {
      Back->Dirty = Back->FailureReported = false;
      return false;
   }
   ImageWriteBackWrote(Back, Now);
   if (Back->FailureReported) return false;
   Back->FailureReported = true;
   return true;
}

static inline void ImageWriteBackForget(struct ImageWriteBack *Back)
{
   Back->Dirty = Back->FailureReported = false;
}
