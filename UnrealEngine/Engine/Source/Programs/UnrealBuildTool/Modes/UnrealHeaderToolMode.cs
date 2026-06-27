// Copyright Epic Games, Inc. All Rights Reserved.

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Threading.Tasks;
using EpicGames.Core;
using EpicGames.UHT.Tables;
using EpicGames.UHT.Tokenizer;
using EpicGames.UHT.Types;
using EpicGames.UHT.Utils;
using Microsoft.Extensions.Logging;
using UnrealBuildBase;

namespace UnrealBuildTool.Modes
{

	/// <summary>
	/// Implement the UHT configuration interface.  Due to the configuration system being fairly embedded into
	/// UBT, the implementation must be part of UBT.
	/// </summary>
	public class UhtConfigImpl : IUhtConfig
	{
		private readonly ConfigHierarchy _ini;

		/// <summary>
		/// Types that have been renamed, treat the old deprecated name as the new name for code generation
		/// </summary>
		private readonly Dictionary<StringView, StringView> _typeRedirectMap;

		/// <summary>
		/// Meta data that have been renamed, treat the old deprecated name as the new name for code generation
		/// </summary>
		private readonly Dictionary<string, string> _metaDataRedirectMap;

		/// <summary>
		/// Supported units in the game
		/// </summary>
		private readonly HashSet<StringView> _units;

		/// <summary>
		/// Special parsed struct names that do not require a prefix
		/// </summary>
		[System.Diagnostics.CodeAnalysis.SuppressMessage("CodeQuality", "IDE0052:Remove unread private members", Justification = "<Pending>")]
		private readonly HashSet<StringView> _structsWithNoPrefix;

		/// <summary>
		/// Special parsed struct names that have a 'T' prefix
		/// </summary>
		private readonly HashSet<StringView> _structsWithTPrefix;

		/// <summary>
		/// Mapping from 'human-readable' macro substring to # of parameters for delegate declarations
		/// Index 0 is 1 parameter, Index 1 is 2, etc...
		/// </summary>
		private readonly List<StringView> _delegateParameterCountStrings;

		/// <summary>
		/// Default version of generated code. Defaults to oldest possible, unless specified otherwise in config.
		/// </summary>
		private readonly EGeneratedCodeVersion _defaultGeneratedCodeVersion = EGeneratedCodeVersion.V1;

		/// <summary>
		/// Internal version of pointer warning for native pointers
		/// </summary>
		private readonly UhtIssueBehaviorSet _nativePointerMemberBehavior = new(UhtIssueBehavior.AllowSilently, UhtIssueBehavior.AllowSilently, UhtIssueBehavior.AllowSilently);

		/// <summary>
		/// Internal version of pointer warning for object pointers in the engine
		/// </summary>
		private readonly UhtIssueBehaviorSet _objectPtrMemberBehavior = new(UhtIssueBehavior.AllowSilently, UhtIssueBehavior.AllowSilently, UhtIssueBehavior.AllowSilently);

		/// <summary>
		/// Internal version of behavior when there is a missing generated header include
		/// </summary>
		private readonly UhtIssueBehaviorSet _missingGeneratedHeaderIncludeBehavior = new(UhtIssueBehavior.AllowSilently, UhtIssueBehavior.AllowSilently);

		/// <summary>
		/// Internal version of the behavior set when the underlying type of a regular and namespaced enum isn't set
		/// </summary>
		private readonly UhtIssueBehaviorSet _enumUnderlyingTypeNotSet = new(UhtIssueBehavior.AllowSilently, UhtIssueBehavior.AllowSilently);

		/// <summary>
		/// If true, deprecation warnings should be shown
		/// </summary>
		private readonly bool _showDeprecations = true;

		/// <summary>
		/// If true, UObject properties are enabled in RigVM
		/// </summary>
		private readonly bool _areRigVMUObjectPropertiesEnabled = false;

		/// <summary>
		/// If true, UInterface properties are enabled in RigVM
		/// </summary>
		private readonly bool _areRigVMUInterfacePropertiesEnabled = false;

		/// <summary>
		/// Collection of known documentation policies
		/// </summary>
		public Dictionary<string, UhtDocumentationPolicy> _documentationPolicies = new(StringComparer.OrdinalIgnoreCase);

		/// <summary>
		/// Default documentation policy (usually empty)
		/// </summary>
		private readonly string _defaultDocumentationPolicy = "";

		/// <summary>
		/// Settings to use for the development status
		/// </summary>
		public string _valkyrieDevelopmentStatusKey = "Valkyrie_DevelopmentStatus";

		/// <summary>
		/// Settings to use for the development status
		/// </summary>
		public string _valkyrieDevelopmentStatusValueExperimental = "Experimental";

		/// <summary>
		/// Settings to use for the deprecation status
		/// </summary>
		public string _valkyrieDeprecationStatusKey = "Valkyrie_DeprecationStatus";

		/// <summary>
		/// Settings to use for the deprecation status
		/// </summary>
		public string _valkyrieDeprecationStatusValueDeprecated = "Deprecated";

		#region IUhtConfig Implementation
		/// <inheritdoc/>
		public EGeneratedCodeVersion DefaultGeneratedCodeVersion => _defaultGeneratedCodeVersion;

		/// <inheritdoc/>
		public UhtIssueBehaviorSet NativePointerMemberBehavior => _nativePointerMemberBehavior;

		/// <inheritdoc/>
		public UhtIssueBehaviorSet ObjectPtrMemberBehavior => _objectPtrMemberBehavior;

		/// <summary>
		/// If true, UObject properties are enabled in RigVM
		/// </summary>
		public bool AreRigVMUObjectPropertiesEnabled => _areRigVMUObjectPropertiesEnabled;

		/// <summary>
		/// If true, UInterface properties are enabled in RigVM
		/// </summary>
		public bool AreRigVMUInterfacePropertiesEnabled => _areRigVMUInterfacePropertiesEnabled;

		/// <summary>
		/// If true, deprecation warnings should be shown
		/// </summary>
		public bool ShowDeprecations => _showDeprecations;

		/// <inheritdoc/>
		public UhtIssueBehaviorSet MissingGeneratedHeaderIncludeBehavior => _missingGeneratedHeaderIncludeBehavior;

		/// <inheritdoc/>
		public UhtIssueBehaviorSet EnumUnderlyingTypeNotSet => _enumUnderlyingTypeNotSet;

		/// <inheritdoc/>
		public IReadOnlyDictionary<string, UhtDocumentationPolicy> DocumentationPolicies => _documentationPolicies;

		/// <inheritdoc/>
		public string DefaultDocumentationPolicy => _defaultDocumentationPolicy;

		/// <inheritdoc/>
		public string ValkyrieDevelopmentStatusKey => _valkyrieDevelopmentStatusKey;

		/// <inheritdoc/>
		public string ValkyrieDevelopmentStatusValueExperimental => _valkyrieDevelopmentStatusValueExperimental;

		/// <inheritdoc/>
		public string ValkyrieDeprecationStatusKey => _valkyrieDeprecationStatusKey;

		/// <inheritdoc/>
		public string ValkyrieDeprecationStatusValueDeprecated => _valkyrieDeprecationStatusValueDeprecated;

		/// <inheritdoc/>
		public void RedirectTypeIdentifier(ref UhtToken Token)
		{
			if (!Token.IsIdentifier())
			{
				throw new Exception("Attempt to redirect type identifier when the token isn't an identifier.");
			}

			if (_typeRedirectMap.TryGetValue(Token.Value, out StringView Redirect))
			{
				Token.Value = Redirect;
			}
		}

		/// <inheritdoc/>
		public bool RedirectMetaDataKey(string Key, out string NewKey)
		{
			if (_metaDataRedirectMap.TryGetValue(Key, out string? Redirect))
			{
				NewKey = Redirect;
				return Key != NewKey;
			}
			else
			{
				NewKey = Key;
				return false;
			}
		}

		/// <inheritdoc/>
		public bool IsValidUnits(StringView Units)
		{
			return _units.Contains(Units);
		}

		/// <inheritdoc/>
		public bool IsStructWithTPrefix(StringView Name)
		{
			return _structsWithTPrefix.Contains(Name);
		}

		/// <inheritdoc/>
		public int FindDelegateParameterCount(StringView DelegateMacro)
		{
			for (int Index = 0, Count = _delegateParameterCountStrings.Count; Index < Count; ++Index)
			{
				if (DelegateMacro.Span.Contains(_delegateParameterCountStrings[Index].Span, StringComparison.Ordinal))
				{
					return Index;
				}
			}
			return -1;
		}

		/// <inheritdoc/>
		public StringView GetDelegateParameterCountString(int Index)
		{
			return Index >= 0 ? _delegateParameterCountStrings[Index] : "";
		}

		/// <inheritdoc/>
		public bool IsExporterEnabled(string Name)
		{
			_ini.GetBool("UnrealHeaderTool", Name, out bool Value);
			return Value;
		}

		/// <inheritdoc/>
		public void Write(IMemoryWriter writer)
		{
			writer.WriteDictionary(_typeRedirectMap, (writer, key) => writer.WriteString(key.ToString()), (writer, value) => writer.WriteString(value.ToString()));
			writer.WriteDictionary(_metaDataRedirectMap, (writer, key) => writer.WriteString(key), (writer, value) => writer.WriteString(value));
			writer.WriteVariableLengthArray(_structsWithNoPrefix.ToArray(), (writer, key) => writer.WriteString(key.ToString()));
			writer.WriteVariableLengthArray(_structsWithTPrefix.ToArray(), (writer, value) => writer.WriteString(value.ToString()));
			writer.WriteVariableLengthArray(_units.ToArray(), (writer, value) => writer.WriteString(value.ToString()));
			writer.WriteVariableLengthArray(_delegateParameterCountStrings.ToArray(), (writer, value) => writer.WriteString(value.ToString()));
			writer.WriteInt32((int)_defaultGeneratedCodeVersion);
			_nativePointerMemberBehavior.Write(writer);
			_objectPtrMemberBehavior.Write(writer);
			_missingGeneratedHeaderIncludeBehavior.Write(writer);
			_enumUnderlyingTypeNotSet.Write(writer);
			writer.WriteBoolean(_areRigVMUObjectPropertiesEnabled);
			writer.WriteBoolean(_areRigVMUInterfacePropertiesEnabled);
			writer.WriteBoolean(_showDeprecations);
			writer.WriteDictionary(_documentationPolicies, (writer, key) => writer.WriteString(key.ToString()), (writer, value) => value.Write(writer));
			writer.WriteString(_defaultDocumentationPolicy);
			writer.WriteString(_valkyrieDevelopmentStatusKey);
			writer.WriteString(_valkyrieDevelopmentStatusValueExperimental);
			writer.WriteString(_valkyrieDeprecationStatusKey);
			writer.WriteString(_valkyrieDeprecationStatusValueDeprecated);
		}
		#endregion

		/// <summary>
		/// Read the UHT configuration
		/// </summary>
		/// <param name="Args">Extra command line arguments</param>
		public UhtConfigImpl(CommandLineArguments Args)
		{
			DirectoryReference ConfigDirectory = DirectoryReference.Combine(Unreal.EngineDirectory, "Programs", "UnrealHeaderTool");
			_ini = ConfigCache.ReadHierarchy(ConfigHierarchyType.Engine, ConfigDirectory, BuildHostPlatform.Current.Platform, "", Args.GetRawArray());

			_typeRedirectMap = GetRedirectsStringView("UnrealHeaderTool", "TypeRedirects", "OldType", "NewType");
			_metaDataRedirectMap = GetRedirectsString("CoreUObject.Metadata", "MetadataRedirects", "OldKey", "NewKey");
			_structsWithNoPrefix = GetHashSet("UnrealHeaderTool", "StructsWithNoPrefix", StringViewComparer.Ordinal);
			_structsWithTPrefix = GetHashSet("UnrealHeaderTool", "StructsWithTPrefix", StringViewComparer.Ordinal);
			_units = GetHashSet("UnrealHeaderTool", "Units", StringViewComparer.OrdinalIgnoreCase);
			_delegateParameterCountStrings = GetList("UnrealHeaderTool", "DelegateParameterCountStrings");
			_defaultGeneratedCodeVersion = GetGeneratedCodeVersion("UnrealHeaderTool", "DefaultGeneratedCodeVersion", EGeneratedCodeVersion.V1);
			_nativePointerMemberBehavior = GetIssueBehaviorSet3("UnrealHeaderTool", "NativePointerMemberBehavior", _nativePointerMemberBehavior);
			_objectPtrMemberBehavior = GetIssueBehaviorSet3("UnrealHeaderTool", "ObjectPtrMemberBehavior", _objectPtrMemberBehavior);
			_missingGeneratedHeaderIncludeBehavior = GetIssueBehaviorSet2("UnrealHeaderTool", "MissingGeneratedHeaderIncludeBehavior", _missingGeneratedHeaderIncludeBehavior);
			_enumUnderlyingTypeNotSet = GetIssueBehaviorSet2("UnrealHeaderTool", "EnumUnderlyingTypeNotSet", _enumUnderlyingTypeNotSet);
			_areRigVMUObjectPropertiesEnabled = GetBoolean("UnrealHeaderTool", "AreRigVMUObjectPropertiesEnabled", false);
			_areRigVMUInterfacePropertiesEnabled = GetBoolean("UnrealHeaderTool", "AreRigVMUInterfacePropertiesEnabled", false);
			_showDeprecations = GetBoolean("UnrealHeaderTool", "ShowDeprecations", true);
			GetDocumentationPolicies("UnrealHeaderTool", "DocumentationPolicies");
			_defaultDocumentationPolicy = GetString("UnrealHeaderTool", "DefaultDocumentationPolicy", "");
		}

		/// <summary>
		/// Read any game configuration for the editor...
		/// </summary>
		/// <param name="ProjectPath"></param>
		public void ProjectSpecificConfigs(string? ProjectPath)
		{
			if (String.IsNullOrEmpty(ProjectPath))
			{
				return;
			}
			DirectoryReference? ProjectDirectory = DirectoryReference.FromString(ProjectPath);
			if (ProjectDirectory == null)
			{
				return;
			}

			ConfigHierarchy editorConfig = ConfigCache.ReadHierarchy(ConfigHierarchyType.Editor, ProjectDirectory, BuildHostPlatform.Current.Platform, "");

			_valkyrieDevelopmentStatusKey = GetString(editorConfig, "/Script/Engine.ValkyrieMetaData", "DevelopmentStatusKey", _valkyrieDevelopmentStatusKey);
			_valkyrieDevelopmentStatusValueExperimental = GetString(editorConfig, "/Script/Engine.ValkyrieMetaData", "DevelopmentStatusValue_Experimental", _valkyrieDevelopmentStatusValueExperimental);
			_valkyrieDeprecationStatusKey = GetString(editorConfig, "/Script/Engine.ValkyrieMetaData", "DeprecationStatusKey", _valkyrieDeprecationStatusKey);
			_valkyrieDeprecationStatusValueDeprecated = GetString(editorConfig, "/Script/Engine.ValkyrieMetaData", "DeprecationStatusValue_Deprecated", _valkyrieDeprecationStatusValueDeprecated);
		}

		private bool GetBoolean(string SectionName, string KeyName, bool bDefault)
		{
			if (_ini.TryGetValue(SectionName, KeyName, out bool value))
			{
				return value;
			}
			return bDefault;
		}

		private string GetString(string SectionName, string KeyName, string Default)
		{
			return GetString(_ini, SectionName, KeyName, Default);
		}

		private static string GetString(ConfigHierarchy config, string SectionName, string KeyName, string Default)
		{
			if (config.TryGetValue(SectionName, KeyName, out string? value))
			{
				return value;
			}
			return Default;
		}

		private UhtIssueBehavior GetIssueBehavior(string SectionName, string KeyName, UhtIssueBehavior Default)
		{
			if (_ini.TryGetValue(SectionName, KeyName, out string? BehaviorStr))
			{
				if (!Enum.TryParse(BehaviorStr, out UhtIssueBehavior Value))
				{
					throw new Exception(String.Format("Unrecognized issue behavior '{0}'", BehaviorStr));
				}
				return Value;
			}
			return Default;
		}

		private UhtIssueBehaviorSet GetIssueBehaviorSet3(string SectionName, string PartialKeyName, UhtIssueBehaviorSet Default)
		{
			return new UhtIssueBehaviorSet(
				GetIssueBehavior(SectionName, $"Engine{PartialKeyName}", Default.Engine),
				GetIssueBehavior(SectionName, $"EnginePlugin{PartialKeyName}", Default.EnginePlugin),
				GetIssueBehavior(SectionName, $"NonEngine{PartialKeyName}", Default.NonEngine));
		}

		private UhtIssueBehaviorSet GetIssueBehaviorSet2(string SectionName, string PartialKeyName, UhtIssueBehaviorSet Default)
		{
			return new UhtIssueBehaviorSet(
				GetIssueBehavior(SectionName, $"Engine{PartialKeyName}", Default.Engine),
				GetIssueBehavior(SectionName, $"NonEngine{PartialKeyName}", Default.NonEngine));
		}

		private EGeneratedCodeVersion GetGeneratedCodeVersion(string SectionName, string KeyName, EGeneratedCodeVersion Default)
		{
			if (_ini.TryGetValue(SectionName, KeyName, out string? BehaviorStr))
			{
				if (!Enum.TryParse(BehaviorStr, out EGeneratedCodeVersion Value))
				{
					throw new Exception(String.Format("Unrecognized generated code version '{0}'", BehaviorStr));
				}
				return Value;
			}
			return Default;
		}

		private Dictionary<StringView, StringView> GetRedirectsStringView(string Section, string Key, string OldKeyName, string NewKeyName)
		{
			Dictionary<StringView, StringView> Redirects = [];

			if (_ini.TryGetValues(Section, Key, out IReadOnlyList<string>? StringList))
			{
				foreach (string Line in StringList)
				{
					if (ConfigHierarchy.TryParse(Line, out Dictionary<string, string>? Properties))
					{
						if (!Properties.TryGetValue(OldKeyName, out string? OldKey))
						{
							throw new Exception(String.Format("Unable to get the {0} from the {1} value", OldKeyName, Key));
						}
						if (!Properties.TryGetValue(NewKeyName, out string? NewKey))
						{
							throw new Exception(String.Format("Unable to get the {0} from the {1} value", NewKeyName, Key));
						}
						Redirects.Add(OldKey, NewKey);
					}
				}
			}
			return Redirects;
		}

		private Dictionary<string, string> GetRedirectsString(string Section, string Key, string OldKeyName, string NewKeyName)
		{
			Dictionary<string, string> Redirects = [];

			if (_ini.TryGetValues(Section, Key, out IReadOnlyList<string>? StringList))
			{
				foreach (string Line in StringList)
				{
					if (ConfigHierarchy.TryParse(Line, out Dictionary<string, string>? Properties))
					{
						if (!Properties.TryGetValue(OldKeyName, out string? OldKey))
						{
							throw new Exception(String.Format("Unable to get the {0} from the {1} value", OldKeyName, Key));
						}
						if (!Properties.TryGetValue(NewKeyName, out string? NewKey))
						{
							throw new Exception(String.Format("Unable to get the {0} from the {1} value", NewKeyName, Key));
						}
						Redirects.Add(OldKey, NewKey);
					}
				}
			}
			return Redirects;
		}

		private List<StringView> GetList(string Section, string Key)
		{
			List<StringView> List = [];

			if (_ini.TryGetValues(Section, Key, out IReadOnlyList<string>? StringList))
			{
				foreach (string Value in StringList)
				{
					List.Add(new StringView(Value));
				}
			}
			return List;
		}

		private HashSet<StringView> GetHashSet(string Section, string Key, StringViewComparer Comparer)
		{
			HashSet<StringView> Set = new(Comparer);

			if (_ini.TryGetValues(Section, Key, out IReadOnlyList<string>? StringList))
			{
				foreach (string Value in StringList)
				{
					Set.Add(new StringView(Value));
				}
			}
			return Set;
		}

		private void GetDocumentationPolicies(string Section, string Key)
		{
			_documentationPolicies["Strict"] = new()
			{
				ClassOrStructCommentRequired = true,
				FunctionToolTipsRequired = true,
				MemberToolTipsRequired = true,
				ParameterToolTipsRequired = true,
				FloatRangesRequired = true,
			};

			_documentationPolicies["None"] = new()
			{
				ClassOrStructCommentRequired = false,
				FunctionToolTipsRequired = false,
				MemberToolTipsRequired = false,
				ParameterToolTipsRequired = false,
				FloatRangesRequired = false,
			};

			if (_ini.TryGetValues(Section, Key, out IReadOnlyList<string>? StringList))
			{
				foreach (string Value in StringList)
				{
					if (ConfigHierarchy.TryParse(Value, out Dictionary<string, string>? Properties))
					{
						if (Properties.TryGetValue("Name", out string? PolicyName))
						{
							if (!_documentationPolicies.TryGetValue(PolicyName, out UhtDocumentationPolicy? Policy))
							{
								Policy = new UhtDocumentationPolicy();
							}
							Policy.ClassOrStructCommentRequired = GetPropertyBool(Properties, "ClassOrStructCommentRequired", Policy.ClassOrStructCommentRequired);
							Policy.FunctionToolTipsRequired = GetPropertyBool(Properties, "FunctionToolTipsRequired", Policy.FunctionToolTipsRequired);
							Policy.MemberToolTipsRequired = GetPropertyBool(Properties, "MemberToolTipsRequired", Policy.MemberToolTipsRequired);
							Policy.ParameterToolTipsRequired = GetPropertyBool(Properties, "ParameterToolTipsRequired", Policy.ParameterToolTipsRequired);
							Policy.FloatRangesRequired = GetPropertyBool(Properties, "FloatRangesRequired", Policy.FloatRangesRequired);
						}
					}
				}
			}
		}

		private static bool GetPropertyBool(Dictionary<string, string> Properties, string Key, bool DefaultValue)
		{
			if (Properties.TryGetValue(Key, out string? PropValueString) && ConfigHierarchy.TryParse(PropValueString, out bool PropValue))
			{
				return PropValue;
			}
			return DefaultValue;
		}
	}

	/// <summary>
	/// Global options for UBT (any modes)
	/// </summary>
	class UhtGlobalOptions
	{
		/// <summary>
		/// User asked for help
		/// </summary>
		[CommandLine(Prefix = "-Help", Description = "Display this help.")]
		[CommandLine(Prefix = "-h")]
		[CommandLine(Prefix = "--help")]
		public bool bGetHelp = false;

		[CommandLine("-Mode", Description = "Optional UBT mode used to invoke the tool")]
		public string? Mode = null;

		/// <summary>
		/// The amount of detail to write to the log
		/// </summary>
		[CommandLine(Prefix = "-Verbose", Value = "Verbose", Description = "Increase output verbosity")]
		[CommandLine(Prefix = "-VeryVerbose", Value = "VeryVerbose", Description = "Increase output verbosity more")]
		public LogEventType LogOutputLevel = LogEventType.Log;

		/// <summary>
		/// Specifies the path to a log file to write. Note that the default mode (eg. building, generating project files) will create a log file by default if this not specified.
		/// </summary>
		[CommandLine(Prefix = "-Log", Description = "Specify a log file location instead of the default Engine/Programs/UnrealHeaderTool/Saved/Logs/UnrealHeaderTool.log")]
		public FileReference? LogFileName = null;

		/// <summary>
		/// Whether to include timestamps in the log
		/// </summary>
		[CommandLine(Prefix = "-Timestamps", Description = "Include timestamps in the log")]
		public bool bLogTimestamps = false;

		/// <summary>
		/// Whether to format messages in MsBuild format
		/// </summary>
		[CommandLine(Prefix = "-FromMsBuild", Description = "Format messages for msbuild")]
		public bool bLogFromMsBuild = false;

		/// <summary>
		/// Disables all logging including the default log location
		/// </summary>
		[CommandLine(Prefix = "-NoLog", Description = "Disable log file creation including the default log file")]
		public bool bNoLog = false;

		[CommandLine("-WarningsAsErrors", Description = "Treat warnings as errors")]
		public bool bWarningsAsErrors = false;

		[CommandLine("-NoGoWide", Description = "Disable concurrent parsing and code generation")]
		public bool bNoGoWide = false;

		[CommandLine("-WriteRef", Description = "Write all the output to a reference directory")]
		public bool bWriteRef = false;

		[CommandLine("-VerifyRef", Description = "Write all the output to a verification directory and compare to the reference output")]
		public bool bVerifyRef = false;

		[CommandLine("-FailIfGeneratedCodeChanges", Description = "Consider any changes to output files as being an error")]
		public bool bFailIfGeneratedCodeChanges = false;

		[CommandLine("-NoOutput", Description = "Do not save any output files other than reference output")]
		public bool bNoOutput = false;

		[CommandLine("-IncludeDebugOutput", Description = "Include extra content in generated output to assist with debugging")]
		public bool bIncludeDebugOutput = false;

		[CommandLine("-NoDefaultExporters", Description = "Disable all default exporters.  Useful for when a specific exporter is to be run")]
		public bool bNoDefaultExporters = false;

		[CommandLine("-EnableAllCodeGen", Description = "Include both the params and constinit code generation")]
		public bool bEnableAllCodeGen = false;

		[CommandLine("-EnableInputCache", Description = "Enable the input cache in read/write mode")]
		public bool bEnableInputCache = false;

		[CommandLine("-EnableInputCacheRead", Description = "Enable the input cache in read mode")]
		public bool bEnableInputCacheRead = false;

		[CommandLine("-EnableInputCacheWrite", Description = "Enable the input cache in write mode")]
		public bool bEnableInputCacheWrite = false;

		[CommandLine("-ReportEmptyHeaders", Description = "Report on any headers that don't contain types but were parsed")]
		public bool bReportEmptyHeaders = false;

		/// <summary>
		/// Initialize the options with the given command line arguments
		/// </summary>
		/// <param name="Arguments"></param>
		public UhtGlobalOptions(CommandLineArguments Arguments)
		{
			Arguments.ApplyTo(this);
		}
	}

	/// <summary>
	/// Invoke UHT
	/// </summary>
	internal sealed class UnrealHeaderToolMode : IToolMode<UnrealHeaderToolMode>
	{
		public static string Name => "UnrealHeaderTool";
		public static ToolModeOptions Options => ToolModeOptions.XmlConfig | ToolModeOptions.BuildPlatforms | ToolModeOptions.ShowExecutionTime;

		/// <summary>
		/// Directory for saved application settings (typically Engine/Programs)
		/// </summary>
		static DirectoryReference? CachedEngineProgramSavedDirectory;

		/// <summary>
		/// The engine programs directory
		/// </summary>
		public static DirectoryReference EngineProgramSavedDirectory
		{
			get
			{
				if (CachedEngineProgramSavedDirectory == null)
				{
					if (Unreal.IsEngineInstalled())
					{
						CachedEngineProgramSavedDirectory = Unreal.UserSettingDirectory ?? DirectoryReference.Combine(Unreal.EngineDirectory, "Programs");
					}
					else
					{
						CachedEngineProgramSavedDirectory = DirectoryReference.Combine(Unreal.EngineDirectory, "Programs");
					}
				}
				return CachedEngineProgramSavedDirectory;
			}
		}

		/// <summary>
		/// Print (incomplete) usage information
		/// </summary>
		/// <param name="ExporterTable">Defined exporters</param>
		/// <param name="Config">Configuration</param>
		private static void PrintUsage(UhtExporterTable ExporterTable, UhtConfigImpl Config)
		{
			Console.WriteLine("UnrealBuildTool -Mode=UnrealHeaderTool [ProjectFile ManifestFile] -OR [\"-Target...\"] [Options]");
			Console.WriteLine("");
			Console.WriteLine("Options:");
			int LongestPrefix = 0;
			foreach (FieldInfo Info in typeof(UhtGlobalOptions).GetFields())
			{
				foreach (CommandLineAttribute Att in Info.GetCustomAttributes<CommandLineAttribute>())
				{
					if (Att.Prefix != null && Att.Description != null)
					{
						LongestPrefix = Att.Prefix.Length > LongestPrefix ? Att.Prefix.Length : LongestPrefix;
					}
				}
			}

			foreach (UhtExporter Generator in ExporterTable)
			{
				LongestPrefix = Generator.Name.Length + 2 > LongestPrefix ? Generator.Name.Length + 2 : LongestPrefix;
			}

			foreach (FieldInfo Info in typeof(UhtGlobalOptions).GetFields())
			{
				foreach (CommandLineAttribute Att in Info.GetCustomAttributes<CommandLineAttribute>())
				{
					if (Att.Prefix != null && Att.Description != null)
					{
						Console.WriteLine($"  {Att.Prefix.PadRight(LongestPrefix)} :  {Att.Description}");
					}
				}
			}

			Console.WriteLine("");
			Console.WriteLine("Generators: Prefix with 'no' to disable a generator");
			foreach (UhtExporter Generator in ExporterTable)
			{
				string IsDefault = Config.IsExporterEnabled(Generator.Name) || Generator.Options.HasAnyFlags(UhtExporterOptions.Default) ? " (Default)" : "";
				Console.WriteLine($"  -{Generator.Name.PadRight(LongestPrefix)} :  {Generator.Description}{IsDefault}");
			}
			Console.WriteLine("");
		}

		/// <summary>
		/// Execute the command
		/// </summary>
		/// <param name="Arguments">Command line arguments</param>
		/// <returns>Exit code</returns>
		/// <param name="Logger"></param>
		public async Task<int> ExecuteAsync(CommandLineArguments Arguments, ILogger Logger)
		{
			try
			{

				// Start a time to track runtime
				Stopwatch stopwatch = new();
				stopwatch.Start();

				// Initialize the attributes
				UhtTables Tables = new();

				// Initialize the config
				UhtConfigImpl Config = new(Arguments);

				// Parse the global options
				UhtGlobalOptions Options = new(Arguments);
				int TargetArgumentIndex = -1;
				if (Arguments.GetPositionalArgumentCount() == 0)
				{
					for (int Index = 0; Index < Arguments.Count; ++Index)
					{
						if (Arguments[Index].StartsWith("-Target", StringComparison.OrdinalIgnoreCase))
						{
							TargetArgumentIndex = Index;
							break;
						}
					}
				}
				int RequiredArgCount = TargetArgumentIndex >= 0 ? 0 : 2;
				if (Arguments.GetPositionalArgumentCount() != RequiredArgCount || Options.bGetHelp)
				{
					PrintUsage(Tables.ExporterTable, Config);
					return Options.bGetHelp ? (int)CompilationResult.Succeeded : (int)CompilationResult.OtherCompilationError;
				}

				// Configure the log system
				Log.OutputLevel = Options.LogOutputLevel;
				Log.IncludeTimestamps = Options.bLogTimestamps;
				Log.IncludeProgramNameWithSeverityPrefix = Options.bLogFromMsBuild;

				// Add the log writer if requested. When building a target, we'll create the writer for the default log file later.
				if (!Options.bNoLog)
				{
					if (Options.LogFileName != null)
					{
						Log.AddFileWriter("LogTraceListener", Options.LogFileName);
					}

					if (!Log.HasFileWriter())
					{
						string BaseLogFileName = FileReference.Combine(EngineProgramSavedDirectory, "UnrealHeaderTool", "Saved", "Logs", "UnrealHeaderTool.log").FullName;

						FileReference LogFile = new(BaseLogFileName);
						foreach (string LogSuffix in Arguments.GetValues("-LogSuffix="))
						{
							LogFile = LogFile.ChangeExtension(null) + "_" + LogSuffix + LogFile.GetExtension();
						}

						Log.AddFileWriter("DefaultLogTraceListener", LogFile);
					}
				}

				string? ProjectFile = null;
				string? ManifestPath = null;

				if (TargetArgumentIndex >= 0)
				{
					// Create the build configuration object, and read the settings
					BuildConfiguration BuildConfiguration = new();
					XmlConfig.ApplyTo(BuildConfiguration);
					Arguments.ApplyTo(BuildConfiguration);

					CommandLineArguments LocalArguments = new([Arguments[TargetArgumentIndex]]);
					List<TargetDescriptor> TargetDescriptors = TargetDescriptor.ParseCommandLine(LocalArguments, BuildConfiguration, Logger);
					if (TargetDescriptors.Count == 0)
					{
						Logger.LogError("No target descriptors found.");
						return (int)CompilationResult.OtherCompilationError;
					}

					TargetDescriptor TargetDesc = TargetDescriptors[0];

					// Create the target
					UEBuildTarget Target = UEBuildTarget.Create(TargetDesc, BuildConfiguration, Logger);

					// Create the makefile for the target and export the module information
					using ISourceFileWorkingSet WorkingSet = new EmptySourceFileWorkingSet();

					// Create the makefile
					TargetMakefile Makefile = await Target.BuildAsync(BuildConfiguration, WorkingSet, TargetDesc, Logger, true);

					FileReference ModuleInfoFileName = UHTExecution.GetUHTModuleInfoFileName(Makefile, Target.TargetName);
					FileReference DepsFileName = UHTExecution.GetUHTDepsFileName(ModuleInfoFileName);
					ManifestPath = ModuleInfoFileName.FullName;
					UHTExecution.WriteUHTManifest(Makefile, Target.TargetName, ModuleInfoFileName, DepsFileName);

					if (Target.ProjectFile != null)
					{
						ProjectFile = Target.ProjectFile.FullName;
					}
				}
				else
				{
					ProjectFile = Arguments.GetPositionalArguments()[0];
					ManifestPath = Arguments.GetPositionalArguments()[1];
				}

				string? ProjectPath = ProjectFile != null ? Path.GetDirectoryName(ProjectFile) : null;
				Config.ProjectSpecificConfigs(ProjectPath);

				UhtSession Session = new(Logger)
				{
					Tables = Tables,
					Config = Config,
					FileManager = new UhtStdFileManager(),
					EngineDirectory = Unreal.EngineDirectory.FullName,
					ProjectFile = ProjectFile,
					ProjectDirectory = String.IsNullOrEmpty(ProjectPath) ? null : ProjectPath,
					ReferenceDirectory = FileReference.Combine(EngineProgramSavedDirectory, "UnrealHeaderTool", "Saved", "ReferenceExports").FullName,
					VerifyDirectory = FileReference.Combine(EngineProgramSavedDirectory, "UnrealHeaderTool", "Saved", "VerifyExports").FullName,
					WarningsAsErrors = Options.bWarningsAsErrors,
					GoWide = !Options.bNoGoWide,
					FailIfGeneratedCodeChanges = Options.bFailIfGeneratedCodeChanges,
					NoOutput = Options.bNoOutput,
					IncludeDebugOutput = Options.bIncludeDebugOutput,
					NoDefaultExporters = Options.bNoDefaultExporters,
					ReportEmptyHeaders = Options.bReportEmptyHeaders,
				};

				if (Options.bEnableInputCacheRead | Options.bEnableInputCache)
				{
					Session.EnableInputCacheRead = true;
				}

				if (Options.bEnableInputCacheWrite | Options.bEnableInputCache)
				{
					Session.EnableInputCacheWrite = true;
				}

				if (Options.bEnableAllCodeGen)
				{
					Session.CompiledInObjectFormat = UhtCompiledInObjectFormat.All;
				}

				if (Options.bWriteRef)
				{
					Session.ReferenceMode = UhtReferenceMode.Reference;
				}
				else if (Options.bVerifyRef)
				{
					Session.ReferenceMode = UhtReferenceMode.Verify;
				}

				// Read and parse
				Session.Run(ManifestPath!, Arguments);
				Session.LogMessages();

				// Generate summary message
				stopwatch.Stop();
				double elapsed = stopwatch.Elapsed.TotalSeconds;
				LogLevel level = elapsed > 5 || Options.Mode != null || Unreal.IsBuildMachine() ? LogLevel.Information : LogLevel.Debug;
				if (!Session.HasErrors || Session.FilesWritten.Count != 0)
				{
					string ActualTargetName = Session.Manifest != null ? Session.Manifest.TargetName : "UnknownTarget";
					if (Session.EnableInputCacheRead || Session.EnableInputCacheWrite)
					{
						Logger.Log(level,
							"UHT processed {TargetName} in {Time} seconds, Generated Files: {FilesWritten}, Source Files Read (disk/cache): {FilesRead}/{FileCacheRead}, Cache Files (written/read): {CacheFilesWritten}/{CacheFilesRead}, Cache Skipped (fnf/version/assembly/config/corrupted) {CacheFilesSkippedFNF}/{CacheFilesSkippedVersion}/{CacheFilesSkippedAssemblyDateTime}/{CacheFilesSkippedConfigChanged}/{CacheFilesSkippedCorrupted}",
							ActualTargetName, stopwatch.Elapsed.TotalSeconds, Session.FilesWritten.Count, Session.FilesRead.Count, Session.FilesCacheRead.Count,
							Session.CacheFilesWritten.Count, Session.CacheFilesRead.Count, Session.CacheFilesSkippedFNF.Count, Session.CacheFilesSkippedVersion.Count,
							Session.CacheFilesSkippedAssemblyDateTime.Count, Session.CacheFilesSkippedConfigChanged.Count, Session.CacheFilesSkippedCorrupted.Count);
					}
					else
					{
						Logger.Log(level, "UHT processed {TargetName} in {Time} seconds ({TotalFilesWritten} generated files written)",
							ActualTargetName, stopwatch.Elapsed.TotalSeconds, Session.FilesWritten.Count);
					}
				}
				if (Session.FilesCacheReadErrors.Count != 0 || Session.FilesCacheWriteErrors.Count != 0)
				{
					Logger.Log(level, "UHT input cache had errors ({TotalFilesCacheReadErrors} file failed deserialization, {TotalFilesCacheReadErrors} files failed serialization)",
						Session.FilesCacheReadErrors.Count, Session.FilesCacheWriteErrors.Count);
				}

				return (int)(Session.HasErrors ? CompilationResult.OtherCompilationError : CompilationResult.Succeeded);
			}
			catch (Exception Ex)
			{
				// Unhandled exception.
				Logger.LogError(Ex, "Unhandled exception: {Ex}", ExceptionUtils.FormatException(Ex));
				Logger.LogDebug(Ex, "Unhandled exception: {Ex}", ExceptionUtils.FormatExceptionDetails(Ex));
				return (int)CompilationResult.OtherCompilationError;
			}
		}
	}
}
