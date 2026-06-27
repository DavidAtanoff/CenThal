// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#if WITH_VERSE_VM || defined(__INTELLISENSE__)

#include "VerseVM/VVMWriteBarrier.h"

namespace Verse
{
template <typename DerivedType>
struct TIntrusiveTree
{
	TWriteBarrier<DerivedType> Parent;
	TWriteBarrier<DerivedType> LastChild;
	TWriteBarrier<DerivedType> Prev;
	TWriteBarrier<DerivedType> Next;

	TIntrusiveTree(FAccessContext Context, DerivedType* Parent, EWriteMode WriteMode = EWriteMode::Default)
		: Parent(Context, Parent)
	{
		switch (WriteMode)
		{
			case EWriteMode::Default:
				AttachToParent<EWriteMode::Default>(Context);
				break;
			case EWriteMode::NonTransactional:
				AttachToParent<EWriteMode::NonTransactional>(Context);
				break;
			case EWriteMode::Transactional:
				AttachToParent<EWriteMode::Transactional>(Context);
				break;
		}
	}

	template <EWriteMode WriteMode>
	void Detach(FAllocationContext Context)
	{
		if (Parent && Parent->LastChild.Get() == This())
		{
			V_DIE_IF(Next);
			Parent->LastChild.template Set<WriteMode>(Context, Prev.Get());
		}
		if (Prev)
		{
			V_DIE_UNLESS(Prev->Next.Get() == This());
			Prev->Next.template Set<WriteMode>(Context, Next.Get());
		}
		if (Next)
		{
			V_DIE_UNLESS(Next->Prev.Get() == This());
			Next->Prev.template Set<WriteMode>(Context, Prev.Get());
		}
		Prev.template Reset<WriteMode>();
		Next.template Reset<WriteMode>();
	}

	void Detach(FAllocationContext Context)
	{
		Detach<EWriteMode::NonTransactional>(::MoveTemp(Context));
	}

	void DetachTransactionally(FAllocationContext Context)
	{
		Detach<EWriteMode::Transactional>(::MoveTemp(Context));
	}

	// Visit each element of the subtree rooted at `this`.
	// This visits nodes in pre-order: parents are visited before children.
	template <typename FunctionType>
	void ForEachInPreOrder(FunctionType&& Function)
	{
		if (LIKELY(!LastChild.Get()))
		{
			Function(*This());
			return;
		}

		TArray<DerivedType*> ToVisit;
		ToVisit.Push(This());
		while (ToVisit.Num())
		{
			DerivedType* Derived = ToVisit.Pop();
			Function(*Derived);
			for (DerivedType* Child = Derived->LastChild.Get(); Child; Child = Child->Prev.Get())
			{
				ToVisit.Push(Child);
			}
		}
	}

	DerivedType* This()
	{
		return static_cast<DerivedType*>(this);
	}

	template <typename TVisitor>
	void VisitReferencesImpl(TVisitor& Visitor)
	{
		Visitor.Visit(Parent, TEXT("Parent"));
		Visitor.Visit(LastChild, TEXT("LastChild"));
		Visitor.Visit(Prev, TEXT("Prev"));
		Visitor.Visit(Next, TEXT("Next"));
	}

private:
	template <EWriteMode WriteMode>
	void AttachToParent(FAccessContext Context)
	{
		if (Parent)
		{
			if (Parent->LastChild)
			{
				Prev.template Set<WriteMode>(Context, Parent->LastChild.Get());
				Parent->LastChild->Next.template Set<WriteMode>(Context, This());
			}
			Parent->LastChild.template Set<WriteMode>(Context, This());
		}
	}
};
} // namespace Verse

#endif
