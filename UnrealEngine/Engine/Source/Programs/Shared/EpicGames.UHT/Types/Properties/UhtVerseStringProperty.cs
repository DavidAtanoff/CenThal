// Copyright Epic Games, Inc. All Rights Reserved.

using System;
using System.Collections.Generic;
using System.Diagnostics.CodeAnalysis;
using System.Text;
using EpicGames.UHT.Tables;
using EpicGames.UHT.Tokenizer;
using EpicGames.UHT.Utils;

namespace EpicGames.UHT.Types
{

	/// <summary>
	/// FVerseStringProperty
	/// </summary>
	[UnrealHeaderTool]
	[UhtEngineClass(Name = "VerseStringProperty", IsProperty = true)]
	public class UhtVerseStringProperty : UhtProperty
	{

		/// <summary>
		/// Defines the type of embedded character type.  Currently only u8 is used.
		/// </summary>
		public UhtProperty CharTypeProperty { get; init; }

		/// <inheritdoc/>
		public override string EngineClassName => "VerseStringProperty";

		/// <inheritdoc/>
		protected override string CppTypeText => "FVerseString";

		/// <inheritdoc/>
		protected override string PGetMacroText => "PROPERTY";

		/// <inheritdoc/>
		protected override UhtPGetArgumentType PGetTypeArgument => UhtPGetArgumentType.EngineClass;

		/// <inheritdoc/>
		protected override string CodeGenParamsStruct => "FVerseStringPropertyParams";

		/// <inheritdoc/>
		protected override string CodeGenParamsFlags => "UECodeGen_Private::EPropertyGenFlags::VerseString";

		/// <summary>
		/// Construct a new property
		/// </summary>
		/// <param name="propertySettings">Property settings</param>
		public UhtVerseStringProperty(UhtPropertySettings propertySettings) : base(propertySettings)
		{
			PropertyCaps |= UhtPropertyCaps.SupportsVerse;
			UhtPropertySettings charTypePropertySettings = new();
			charTypePropertySettings.Reset(this, 0, PropertyCategory, 0);
			charTypePropertySettings.SourceName = propertySettings.SourceName;
			CharTypeProperty = new UhtByteProperty(propertySettings);
			CharTypeProperty.PropertyCaps |= UhtPropertyCaps.SupportsVerse; // needed so we don't generate errors
		}

		/// <summary>
		/// Construct a type from the cache
		/// </summary>
		/// <param name="reader">Reader</param>
		/// <param name="outer">Outer type</param>
		public UhtVerseStringProperty(UhtInputCacheReader reader, UhtType outer) : base(reader, outer)
		{
			CharTypeProperty = (reader.ReadType() as UhtProperty)!;
		}

		/// <summary>
		/// Write the output type
		/// </summary>
		/// <param name="writer"></param>
		public override void Write(UhtInputCacheWriter writer)
		{
			base.Write(writer);
			writer.WriteType(CharTypeProperty);
		}

		/// <inheritdoc/>
		public override StringBuilder AppendText(StringBuilder builder, UhtPropertyTextType textType, bool isTemplateArgument)
		{
			switch (textType)
			{
				case UhtPropertyTextType.VerseMangledType:
					builder.Append("[]char");
					break;

				default:
					base.AppendText(builder, textType, isTemplateArgument);
					break;
			}
			return builder;
		}

		/// <inheritdoc/>
		protected override bool NeedsGCBarrierWhenPassedToFunctionImpl(UhtFunction function)
		{
			return CharTypeProperty.NeedsGCBarrierWhenPassedToFunction(function);
		}

		/// <inheritdoc/>
		public override void Validate(UhtStruct outerStruct, UhtProperty outermostProperty, UhtValidationOptions options)
		{
			base.Validate(outerStruct, outermostProperty, options);
			CharTypeProperty.Validate(outerStruct, outermostProperty, options);
		}

		/// <inheritdoc/>
		public override bool ScanForInstancedReferenced(bool deepScan)
		{
			return CharTypeProperty.ScanForInstancedReferenced(deepScan);
		}

		///<inheritdoc/>
		public override bool IsAllowedInOptionalClass([NotNullWhen(false)] out string? propPath)
		{
			return CharTypeProperty.IsAllowedInOptionalClass(out propPath);
		}

		/// <inheritdoc/>
		public override IEnumerable<UhtType> EnumerateReferencedTypes()
		{
			foreach (UhtType type in CharTypeProperty.EnumerateReferencedTypes())
			{
				yield return type;
			}
		}

		/// <inheritdoc/>
		public override StringBuilder AppendNullConstructorArg(StringBuilder builder, bool isInitializer)
		{
			builder.AppendPropertyText(this, UhtPropertyTextType.Construction).Append("()");
			return builder;
		}

		/// <inheritdoc/>
		public override IEnumerable<UhtChildProperty> EnumerateChildProperties(UhtCppIdentifier identifier, UhtChildPropertyOrder order)
		{
			yield return new(CharTypeProperty, identifier.AppendSuffix("_Inner"));
		}

		/// <inheritdoc/>
		public override void AppendObjectHashes(StringBuilder builder, int startingLength, IUhtPropertyMemberContext context)
		{
			CharTypeProperty.AppendObjectHashes(builder, startingLength, context);
		}

		/// <inheritdoc/>
		public override bool SanitizeDefaultValue(IUhtTokenReader defaultValueReader, StringBuilder innerDefaultValue)
		{
			return false;
		}

		/// <inheritdoc/>
		public override bool IsSameType(UhtProperty other)
		{
			if (other is UhtVerseStringProperty otherString)
			{
				return CharTypeProperty.IsSameType(otherString.CharTypeProperty);
			}
			return false;
		}

		[UhtPropertyType(Keyword = "verse::string", Options = UhtPropertyTypeOptions.Simple | UhtPropertyTypeOptions.Immediate)]
		[UhtPropertyType(Keyword = "FVerseString", Options = UhtPropertyTypeOptions.Simple | UhtPropertyTypeOptions.Immediate)]
		[SuppressMessage("CodeQuality", "IDE0051:Remove unused private members", Justification = "Attribute accessed method")]
		[SuppressMessage("Style", "IDE0060:Remove unused parameter", Justification = "Attribute accessed method")]
		private static UhtVerseStringProperty? VerseStringProperty(UhtPropertyResolveArgs args)
		{
			return new UhtVerseStringProperty(args.PropertySettings);
		}
	}
}
