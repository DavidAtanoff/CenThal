// Copyright Epic Games, Inc. All Rights Reserved.

using System;
using System.Diagnostics.CodeAnalysis;
using System.Text;
using EpicGames.Core;
using EpicGames.UHT.Tables;
using EpicGames.UHT.Tokenizer;
using EpicGames.UHT.Utils;

namespace EpicGames.UHT.Types
{

	/// <summary>
	/// FUtf8StrProperty
	/// </summary>
	[UnrealHeaderTool]
	[UhtEngineClass(Name = "Utf8StrProperty", IsProperty = true)]
	public class UhtUtf8StrProperty : UhtProperty
	{
		/// <inheritdoc/>
		public override string EngineClassName => "Utf8StrProperty";

		/// <inheritdoc/>
		protected override string CppTypeText => "FUtf8String";

		/// <inheritdoc/>
		protected override string PGetMacroText => "PROPERTY";

		/// <inheritdoc/>
		protected override UhtPGetArgumentType PGetTypeArgument => UhtPGetArgumentType.EngineClass;

		/// <inheritdoc/>
		protected override string CodeGenParamsStruct => "FUtf8StrPropertyParams";

		/// <inheritdoc/>
		protected override string CodeGenParamsFlags => "UECodeGen_Private::EPropertyGenFlags::Utf8Str";

		/// <summary>
		/// Construct a new property
		/// </summary>
		/// <param name="propertySettings">Property settings</param>
		public UhtUtf8StrProperty(UhtPropertySettings propertySettings) : base(propertySettings)
		{
			// Other caps not supported until engine support catches up
			PropertyCaps |= UhtPropertyCaps.PassCppArgsByRef; // | UhtPropertyCaps.CanExposeOnSpawn | UhtPropertyCaps.IsParameterSupportedByBlueprint |
															  //UhtPropertyCaps.IsMemberSupportedByBlueprint | UhtPropertyCaps.SupportsRigVM;
		}

		/// <summary>
		/// Construct a type from the cache
		/// </summary>
		/// <param name="reader">Reader</param>
		/// <param name="outer">Outer type</param>
		public UhtUtf8StrProperty(UhtInputCacheReader reader, UhtType outer) : base(reader, outer)
		{
		}

		/// <summary>
		/// Write the output type
		/// </summary>
		/// <param name="writer"></param>
		public override void Write(UhtInputCacheWriter writer)
		{
			base.Write(writer);
		}

		/// <inheritdoc/>
		public override StringBuilder AppendNullConstructorArg(StringBuilder builder, bool isInitializer)
		{
			builder.Append("TEXT(\"\")");
			return builder;
		}

		/// <inheritdoc/>
		public override bool SanitizeDefaultValue(IUhtTokenReader defaultValueReader, StringBuilder innerDefaultValue)
		{
			StringView value = defaultValueReader.GetTypedConstString("FUtf8String");
			innerDefaultValue.Append(value);
			return true;
		}

		/// <inheritdoc/>
		public override bool IsSameType(UhtProperty other)
		{
			return other is UhtUtf8StrProperty;
		}

		/// <inheritdoc/>
		protected override void ValidateFunctionArgument(UhtFunction function, UhtValidationOptions options)
		{
			base.ValidateFunctionArgument(function, options);

			if (function.FunctionFlags.HasAnyFlags(EFunctionFlags.Net))
			{
				if (!function.FunctionFlags.HasAnyFlags(EFunctionFlags.NetRequest))
				{
					if (RefQualifier != UhtPropertyRefQualifier.ConstRef && !IsStaticArray)
					{
						this.LogError("Replicated FUtf8String parameters must be passed by const reference");
					}
				}
			}
		}

		[UhtPropertyType(Keyword = "FUtf8String")]
		[SuppressMessage("CodeQuality", "IDE0051:Remove unused private members", Justification = "Attribute accessed method")]
		[SuppressMessage("Style", "IDE0060:Remove unused parameter", Justification = "Attribute accessed method")]
		private static UhtUtf8StrProperty? Utf8StrProperty(UhtPropertyResolveArgs args)
		{
			UhtPropertySettings propertySettings = args.PropertySettings;
			IUhtTokenReader tokenReader = args.TokenReader;

			if (!args.SkipExpectedType())
			{
				return null;
			}
			UhtUtf8StrProperty property = new(propertySettings);
			if (property.PropertyCategory != UhtPropertyCategory.Member)
			{
				if (tokenReader.TryOptional('&'))
				{
					if (property.PropertyFlags.HasAnyFlags(EPropertyFlags.ConstParm))
					{
						property.PropertyFlags &= ~EPropertyFlags.ConstParm;

						// We record here that we encountered a const reference, because we need to remove that information from flags for code generation purposes.
						property.RefQualifier = UhtPropertyRefQualifier.ConstRef;
					}
					else
					{
						property.PropertyFlags |= EPropertyFlags.OutParm;

						// And we record here that we encountered a non-const reference here too.
						property.RefQualifier = UhtPropertyRefQualifier.NonConstRef;
					}
				}
			}
			return property;
		}
	}
}
