// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#if WITH_VERSE_VM || defined(__INTELLISENSE__)

#include "VerseVM/VVMBytecodesAndCaptures.h"
#include "VerseVM/VVMContextImpl.h"
#include "VerseVM/VVMFrame.h"
#include "VerseVM/VVMTask.h"

namespace Verse
{

COREUOBJECT_API extern FOpErr StopInterpreterSentry;

template <typename VFrameCallback, typename NativeFrameCallback>
void FNativeFrame::WalkCallstackFrames(const FOp* InPC, VFrame* InFrame, VTask* InTask, VFrameCallback FrameCallback, NativeFrameCallback NativeCallback) const
{
	for (const FNativeFrame* NativeFrame = this; NativeFrame; NativeFrame = NativeFrame->PreviousNativeFrame)
	{
		while (InFrame)
		{
			if (InPC != &StopInterpreterSentry)
			{
				FrameCallback(InPC, InFrame);
			}

			if (InTask && InFrame == InTask->RootFrame.Get())
			{
				// InTask-spawn boundary: pivot to parent InTask's YieldFrame.
				InPC = InTask->YieldPC;
				InFrame = InTask->YieldFrame.Get();
				InTask = InTask->YieldTask.Get();
			}
			else
			{
				InPC = InFrame->CallerPC;
				InFrame = InFrame->CallerFrame.Get();
			}
		}

		NativeCallback(NativeFrame);

		InPC = NativeFrame->CallerPC;
		InFrame = NativeFrame->CallerFrame;
		InTask = NativeFrame->Task;
	}
}

} // namespace Verse

#endif // WITH_VERSE_VM || defined(__INTELLISENSE__)
