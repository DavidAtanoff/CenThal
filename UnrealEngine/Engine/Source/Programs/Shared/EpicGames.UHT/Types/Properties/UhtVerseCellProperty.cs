// Copyright Epic Games, Inc. All Rights Reserved.

using System.Diagnostics.CodeAnalysis;
using System.Text;
using EpicGames.UHT.Tables;
using EpicGames.UHT.Tokenizer;
using EpicGames.UHT.Utils;

namespace EpicGames.UHT.Types
{

	/// <summary>
	/// FVCellProperty
	/// </summary>
	[UnrealHeaderTool]
	[UhtEngineClass(Name = "VerseCellProperty", IsProperty = true)]
	public class UhtVerseCellProperty : UhtProperty
	{
		/// <inheritdoc/>
		public override string EngineClassName => "VCellProperty";

		/// <inheritdoc/>
		protected override string CppTypeText => "Verse::TWriteBarrier";

		/// <inheritdoc/>
		protected override string PGetMacroText => "PROPERTY";

		/// <inheritdoc/>
		protected override UhtPGetArgumentType PGetTypeArgument => UhtPGetArgumentType.EngineClass;

		/// <inheritdoc/>
		protected override string CodeGenParamsStruct => "FVerseValuePropertyParams";

		/// <inheritdoc/>
		protected override string CodeGenParamsFlags => "UECodeGen_Private::EPropertyGenFlags::VerseCell";

		/// <summary>
		/// Referenced VCell type
		/// </summary>
		public string Cell { get; set; }

		/// <summary>
		/// Construct a new property
		/// </summary>
		/// <param name="propertySettings">Property settings</param>
		/// <param name="type">Referenced VCell type</param>
		public UhtVerseCellProperty(UhtPropertySettings propertySettings, string type) : base(propertySettings)
		{
			Cell = type;
		}

		/// <summary>
		/// Construct a type from the cache
		/// </summary>
		/// <param name="reader">Reader</param>
		/// <param name="outer">Outer type</param>
		public UhtVerseCellProperty(UhtInputCacheReader reader, UhtType outer) : base(reader, outer)
		{
			Cell = reader.ReadString();
		}

		/// <summary>
		/// Write the output type
		/// </summary>
		/// <param name="writer"></param>
		public override void Write(UhtInputCacheWriter writer)
		{
			base.Write(writer);
			writer.WriteString(Cell);
		}

		/// <inheritdoc/>
		public override StringBuilder AppendText(StringBuilder builder, UhtPropertyTextType textType, bool isTemplateArgument)
		{
			return builder.Append("Verse::TWriteBarrier<").Append(Cell).Append('>');
		}

		/// <inheritdoc/>
		public override StringBuilder AppendNullConstructorArg(StringBuilder builder, bool isInitializer)
		{
			builder.AppendPropertyText(this, UhtPropertyTextType.Construction).Append("()");
			return builder;
		}

		/// <inheritdoc/>
		public override bool SanitizeDefaultValue(IUhtTokenReader defaultValueReader, StringBuilder innerDefaultValue)
		{
			return false;
		}

		/// <inheritdoc/>
		public override bool IsSameType(UhtProperty other)
		{
			return other is UhtVerseCellProperty;
		}

		[UhtPropertyType(Keyword = "Verse::TWriteBarrier", Options = UhtPropertyTypeOptions.Immediate)]
		[UhtPropertyType(Keyword = "TWriteBarrier", Options = UhtPropertyTypeOptions.Immediate)]
		[SuppressMessage("CodeQuality", "IDE0051:Remove unused private members", Justification = "Attribute accessed method")]
		[SuppressMessage("Style", "IDE0060:Remove unused parameter", Justification = "Attribute accessed method")]
		private static UhtVerseCellProperty? VerseProperty(UhtPropertyResolveArgs args)
		{
			if (args.PropertySettings.PropertyCategory != UhtPropertyCategory.Member)
			{
				args.TokenReader.LogError("TWriteBarrier properties cannot be used as function parameters or returns");
			}

			if (!args.SkipExpectedType())
			{
				return null;
			}

			UhtToken identifier = new();
			args.TokenReader
				.Require("<")
				.OptionalNamespace("Verse")
				.RequireIdentifier((ref UhtToken token) => { identifier = token; })
				.Require(">");

			UhtVerseCellProperty property = new(args.PropertySettings, identifier.ToString());
			return property;
		}
	}
}
