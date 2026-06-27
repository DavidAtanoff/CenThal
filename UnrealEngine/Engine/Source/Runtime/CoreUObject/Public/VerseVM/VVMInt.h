// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#if WITH_VERSE_VM || defined(__INTELLISENSE__)

#include "Containers/StringFwd.h"
#include "CoreTypes.h"
#include "HAL/Platform.h"
#include "Misc/AssertionMacros.h"
#include "VVMCell.h"
#include "VVMContext.h"
#include "VVMHeapInt.h"
#include "VVMValue.h"
#include "VerseVM/VVMFloat.h"
#include <cstdint>

namespace Verse
{
struct AUTORTFM_DISABLE VInt : VValue
{
	// Be careful using this! Only classes expecting a uninitialized int should use this such as:
	//	TWriteBarrier, VIntType, etc.
	AUTORTFM_ENABLE explicit VInt()
		: VValue()
	{
	}
	AUTORTFM_ENABLE VInt(const VInt&) = default;
	AUTORTFM_ENABLE explicit VInt(VValue InValue)
		: VValue(InValue)
	{
		checkSlow(InValue.IsInt());
	}
	AUTORTFM_ENABLE explicit VInt(int32 InInt32)
		: VValue(VValue::FromInt32(InInt32))
	{
	}
	VInt(int64) = delete; // nb: use a constructor that takes F*Context explicitly
	VInt(FAllocationContext Context, int64 Int64)
		: VValue(Int64 >= INT32_MIN && Int64 <= INT32_MAX
					 ? VValue::FromInt32(static_cast<int32>(Int64))
					 : VHeapInt::FromInt64(Context, Int64))
	{
	}
	AUTORTFM_ENABLE VInt(VHeapInt& N)
		: VValue(N.IsInt32()
					 ? VValue::FromInt32(N.AsInt32())
					 : VValue(N))
	{
	}

	bool IsZero() const
	{
		if (IsInt32())
		{
			return AsInt32() == 0;
		}
		VHeapInt& HeapInt = StaticCast<VHeapInt>();
		return HeapInt.IsZero();
	}

	bool IsNegative() const
	{
		if (IsInt32())
		{
			return AsInt32() < 0;
		}
		VHeapInt& HeapInt = StaticCast<VHeapInt>();
		return HeapInt.GetSign();
	}

	AUTORTFM_ENABLE bool IsInt64() const;
	AUTORTFM_ENABLE int64 AsInt64() const;

	AUTORTFM_ENABLE bool IsUint32() const;
	AUTORTFM_ENABLE uint32 AsUint32() const;

	AUTORTFM_ENABLE VFloat ConvertToFloat() const;

	static VInt Add(FAllocationContext Context, VInt Lhs, VInt Rhs);
	static VInt Sub(FAllocationContext Context, VInt Lhs, VInt Rhs);
	static VInt Mul(FAllocationContext Context, VInt Lhs, VInt Rhs);
	static VInt Div(FAllocationContext Context, VInt Lhs, VInt Rhs, bool* bOutHasNonZeroRemainder = nullptr);
	static VInt Mod(FAllocationContext Context, VInt Lhs, VInt Rhs);
	static VInt Neg(FAllocationContext Context, VInt N);
	static VInt Abs(FAllocationContext Context, VInt N);
	static bool Eq(FAllocationContext Context, VInt Lhs, VInt Rhs);
	static bool Eq(FAllocationContext Context, VInt Lhs, VValue Rhs);
	static bool Gt(FAllocationContext Context, VInt Lhs, VInt Rhs);
	static bool Lt(FAllocationContext Context, VInt Lhs, VInt Rhs);
	static bool Gte(FAllocationContext Context, VInt Lhs, VInt Rhs);
	static bool Lte(FAllocationContext Context, VInt Lhs, VInt Rhs);

	friend uint32 GetTypeHash(VInt Int);

	COREUOBJECT_API void AppendDecimalToString(FUtf8StringBuilderBase& Builder, FAllocationContext Context) const;
	COREUOBJECT_API void AppendHexToString(FUtf8StringBuilderBase& Builder) const;
	COREUOBJECT_API TSharedPtr<FJsonValue> ToJSON(FAllocationContext Context, EValueJSONFormat Format) const;

private:
	VFloat ConvertToFloatSlowPath() const;

	static VInt AddSlowPath(FAllocationContext Context, VInt Lhs, VInt Rhs);
	static VInt SubSlowPath(FAllocationContext Context, VInt Lhs, VInt Rhs);
	static VInt MulSlowPath(FAllocationContext Context, VInt Lhs, VInt Rhs);
	static VInt DivSlowPath(FAllocationContext Context, VInt Lhs, VInt Rhs, bool* bOutHasNonZeroRemainder);
	static VInt ModSlowPath(FAllocationContext Context, VInt Lhs, VInt Rhs);
	static VInt NegSlowPath(FAllocationContext Context, VInt N);
	static VInt AbsSlowPath(FAllocationContext Context, VInt N);
	static bool EqSlowPath(FAllocationContext Context, VInt Lhs, VInt Rhs);
	static bool LtSlowPath(FAllocationContext, VInt Lhs, VInt Rhs);
	static bool GtSlowPath(FAllocationContext, VInt Lhs, VInt Rhs);
	static bool LteSlowPath(FAllocationContext, VInt Lhs, VInt Rhs);
	static bool GteSlowPath(FAllocationContext, VInt Lhs, VInt Rhs);

	static VHeapInt& AsHeapInt(FAllocationContext, VInt N);
};

} // namespace Verse
#endif // WITH_VERSE_VM
