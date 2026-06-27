// Copyright Epic Games, Inc. All Rights Reserved.

using System.Text;
using EpicGames.Core;
using EpicGames.UHT.Tables;
using EpicGames.UHT.Utils;

namespace EpicGames.UHT.Types
{
	/// <summary>
	/// FVerseClassProperty
	/// </summary>
	[UnrealHeaderTool]
	[UhtEngineClass(Name = "VerseClassProperty", IsProperty = true)]
	public class UhtVerseClassProperty : UhtClassProperty
	{
		/// <inheritdoc/>
		public override string EngineClassName => "VerseClassProperty";

		/// <inheritdoc/>
		public override bool CodeGenWrapInRestValue => true;

		/// <summary>
		/// Construct a new property
		/// </summary>
		/// <param name="propertySettings">Property settings</param>
		/// <param name="cppForm">Source code form of the property</param>
		/// <param name="referencedClass">Referenced class</param>
		/// <param name="extraFlags">Extra flags to apply to the property.</param>
		public UhtVerseClassProperty(UhtPropertySettings propertySettings, UhtObjectCppForm cppForm, UhtClass referencedClass, EPropertyFlags extraFlags = EPropertyFlags.None)
			: base(new UhtNoValidateConstruct { }, propertySettings, cppForm, referencedClass, extraFlags)
		{
			if (!cppForm.IsValidForVerseClassProperty())
			{
				throw new UhtIceException($"Improper UhtObjectCppForm.{cppForm} for an UhtVerseClassProperty");
			}
		}

		/// <summary>
		/// Construct a type from the cache
		/// </summary>
		/// <param name="reader">Reader</param>
		/// <param name="outer">Outer type</param>
		public UhtVerseClassProperty(UhtInputCacheReader reader, UhtType outer) : base(reader, outer)
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
		protected override StringBuilder AppendParamsDefExtra(StringBuilder builder, IUhtPropertyMemberContext context, UhtCppIdentifier identifier)
		{
			base.AppendParamsDefExtra(builder, context, identifier);
			builder.Append($"{RequiresConcreteValue()}, ");
			builder.Append($"{RequiresCastableValue()}, ");
			return builder;
		}

		/// <inheritdoc/>
		protected override StringBuilder AppendConstInitDefExtra(StringBuilder builder, IUhtPropertyMemberContext context, UhtCppIdentifier identifier)
		{
			base.AppendConstInitDefExtra(builder, context, identifier);
			builder.Append($"{RequiresConcreteValue()}, ");
			builder.Append($"{RequiresCastableValue()}, ");
			return builder;
		}

		/// <summary>
		/// Returns either "true" or "false" if the type is a concrete type
		/// </summary>
		/// <returns></returns>
		protected string RequiresConcreteValue()
		{
			return
				CppForm == UhtObjectCppForm.VerseConcreteType || CppForm == UhtObjectCppForm.VerseConcreteSubtype ||
				CppForm == UhtObjectCppForm.VerseCastableConcreteType || CppForm == UhtObjectCppForm.VerseCastableConcreteSubtype ?
					"true" :
					"false";
		}

		/// <summary>
		/// Returns either "true" or "false" if the type is a castable type
		/// </summary>
		/// <returns></returns>
		protected string RequiresCastableValue()
		{
			return
				CppForm == UhtObjectCppForm.VerseCastableType || CppForm == UhtObjectCppForm.VerseCastableSubtype ||
				CppForm == UhtObjectCppForm.VerseCastableConcreteType || CppForm == UhtObjectCppForm.VerseCastableConcreteSubtype ?
					"true" :
					"false";
		}
	}
}
