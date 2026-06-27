// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#if WITH_VERSE_VM || defined(__INTELLISENSE__)

#include "Templates/TypeHash.h"

#include "VerseVM/VVMRef.h"
#include "VerseVM/VVMRestValue.h"
#include "VerseVM/VVMTask.h"
#include "VerseVM/VVMTransaction.h"

namespace Verse
{
inline void VRestValue::SetTransactionally(FAccessContext Context, VValue NewValue)
{
	checkSlow(!NewValue.IsRoot());
	Value.SetTransactionally(Context, NewValue);
}

inline void VRestValue::SetTrailed(FAllocationContext Context, FTrail& Trail, VValue NewValue)
{
	checkSlow(!NewValue.IsRoot());
	Value.SetTrailed(Context, Trail, NewValue);
}

template <typename UnaryFunction>
void FRefAwaiterHeader::ForEach(const TSet<FRefAwaiter>& Set, UnaryFunction F) const
{
	for (FSetElementId I = Next; I.IsValidId(); I = Set.Get(I).Next)
	{
		F(Set[I]);
	}
}

template <typename UnaryFunction>
bool FRefAwaiterHeader::AnyOf(const TSet<FRefAwaiter>& Set, UnaryFunction F) const
{
	TArray<FSetElementId> ElementIds;
	ElementIds.Reserve(Set.Num());
	for (FSetElementId I = Next; I.IsValidId(); I = Set.Get(I).Next)
	{
		ElementIds.Add(I);
	}
	for (FSetElementId ElementId : ElementIds)
	{
		if (F(Set[ElementId]))
		{
			return true;
		}
	}
	return false;
}

FRefAwaiter::FRefAwaiter(FAccessContext Context, VTask& Task, const FOp& AwaitPC)
	: Task{Context, Task}
	, AwaitPC{&AwaitPC}
{
}

inline bool operator==(const FRefAwaiter& Left, const FRefAwaiter& Right)
{
	return Left.Task == Right.Task && Left.AwaitPC == Right.AwaitPC;
}

inline uint32 GetTypeHash(const FRefAwaiter& Arg)
{
	uint32 Result = 0;
	Result = HashCombineFast(Result, GetTypeHash(Arg.Task));
	Result = HashCombineFast(Result, PointerHash(Arg.AwaitPC));
	return Result;
}

template <typename UnaryFunction>
void VRefRareData::ForEachAwaitTask(UnaryFunction F) const
{
	AutoRTFM::UnreachableIfClosed("#jira SOL-8415");
	AwaiterHeader.ForEach(AwaiterBuffer, [&](const FRefAwaiter& Awaiter) {
		AutoRTFM::UnreachableIfClosed("#jira SOL-8415");
		if (ContainsAwaitTask(*Awaiter.Task, *Awaiter.AwaitPC))
		{
			F(*Awaiter.Task);
		}
	});
}

template <typename UnaryFunction>
bool VRefRareData::AnyAwaitTask(UnaryFunction F) const
{
	AutoRTFM::UnreachableIfClosed("#jira SOL-8415");
	return AwaiterHeader.AnyOf(AwaiterBuffer, [&](const FRefAwaiter& Awaiter) {
		AutoRTFM::UnreachableIfClosed("#jira SOL-8415");
		return ContainsAwaitTask(*Awaiter.Task, *Awaiter.AwaitPC) && F(*Awaiter.Task);
	});
}

inline void VRef::Set(FAllocationContext Context, VValue NewValue)
{
	return Value.SetTransactionally(Context, NewValue);
}
} // namespace Verse
#endif // WITH_VERSE_VM
