// Copyright Epic Games, Inc. All Rights Reserved.

using System.Collections.Generic;
using EpicGames.Core;
using Microsoft.Extensions.Logging;

namespace UnrealBuildTool
{
	/// <summary>
	/// Interface to allow exposing public methods from the toolchain to other assemblies
	/// </summary>
	public interface IAndroidToolChain
	{
		/// <summary>
		/// Returns the Android NDK Level
		/// </summary>
		/// <returns>The NDK Level</returns>
		int GetNdkApiLevelInt(int MinNDK);

		/// <summary>
		/// Returns the Current NDK Version
		/// </summary>
		/// <returns>The NDK Version</returns>
		ulong GetNdkVersionInt();
	}

	/// <summary>
	/// Interface to allow exposing public methods from the Android deployment context to other assemblies
	/// </summary>
	public interface IAndroidDeploy
	{
		/// <summary>
		/// 
		/// </summary>
		bool bFromMSBuild { get; }
		
		/// <summary>
		/// 
		/// </summary>
		bool bNoObbs { get; }
		
		/// <summary>
		/// 
		/// </summary>
		/// <returns></returns>
		bool GetPackageDataInsideApk();

		/// <summary>
		/// 
		/// </summary>
		/// <param name="Configuration"></param>
		/// <param name="bIsArchive"></param>
		/// <param name="bIsFromUAT"></param>
		bool GetDontBundleLibrariesInAPK(UnrealTargetConfiguration Configuration, bool bIsArchive, bool bIsFromUAT);
		
		/// <summary>
		/// 
		/// </summary>
		/// <param name="Architectures"></param>
		/// <param name="inPluginExtraData"></param>
		void SetAndroidPluginData(UnrealArchitectures Architectures, List<string> inPluginExtraData);

		/// <summary>
		/// 
		/// </summary>
		/// <param name="ProjectFile"></param>
		/// <param name="ProjectName"></param>
		/// <param name="ProjectDirectory"></param>
		/// <param name="ExecutablePath"></param>
		/// <param name="EngineDirectory"></param>
		/// <param name="bForDistribution"></param>
		/// <param name="CookFlavor"></param>
		/// <param name="Configuration"></param>
		/// <param name="bIsDataDeploy"></param>
		/// <param name="bSkipGradleBuild"></param>
		/// <param name="bIsArchive"></param>
		/// <returns></returns>
		bool PrepForUATPackageOrDeploy(FileReference ProjectFile, string ProjectName, DirectoryReference ProjectDirectory, string ExecutablePath, string EngineDirectory, bool bForDistribution, string CookFlavor, UnrealTargetConfiguration Configuration, bool bIsDataDeploy, bool bSkipGradleBuild, bool bIsArchive);

		/// <summary>
		/// 
		/// </summary>
		/// <param name="ProjectName"></param>
		/// <param name="ProjectDirectoryFullName"></param>
		/// <param name="Type"></param>
		/// <param name="bIsEmbedded"></param>
		bool SavePackageInfo(string ProjectName, string ProjectDirectoryFullName, TargetType Type, bool bIsEmbedded);
	}

	/// <summary>
	/// Public Android functions exposed to UAT
	/// </summary>
	public static class AndroidExports
	{
		private static AndroidToolChain? CachedToolChain;
		private static AndroidToolChain GetToolChain(FileReference? ProjectFile, ILogger Logger)
		{
			if (CachedToolChain == null || !CachedToolChain.Equals(ProjectFile, Logger))
			{
				CachedToolChain = new AndroidToolChain(ProjectFile, Logger);
			}

			return CachedToolChain;
		}

		/// <summary>
		///
		/// </summary>
		/// <param name="ProjectFile"></param>
		/// <param name="InForcePackageData"></param>
		/// <returns></returns>
		public static IAndroidDeploy CreateDeploymentHandler(FileReference ProjectFile, bool InForcePackageData)
		{
			return new UEDeployAndroid(ProjectFile, InForcePackageData, Log.Logger);
		}

		/// <summary>
		///
		/// </summary>
		/// <returns></returns>
		public static bool ShouldMakeSeparateApks()
		{
			return UEDeployAndroid.ShouldMakeSeparateApks();
		}

		/// <summary>
		///
		/// </summary>
		/// <param name="NDKArch"></param>
		/// <returns></returns>
		public static UnrealArch GetUnrealArch(string NDKArch)
		{
			return UEDeployAndroid.GetUnrealArch(NDKArch);
		}

		/// <summary>
		///
		/// </summary>
		/// <param name="SourceFile"></param>
		/// <param name="TargetFile"></param>
		/// <param name="Logger">Logger for output</param>
		/// <param name="bStripAll">Whether to strip unneeded symbols or just debug symbols</param>
		public static void StripSymbols(FileReference SourceFile, FileReference TargetFile, ILogger Logger, bool bStripAll = false)
		{
			// This forces ToolChain's static init to run, which will populate the strip binary paths
			if (CachedToolChain == null)
			{
				GetToolChain(null, Logger);
			}

			AndroidToolChain.StripSymbols(SourceFile, TargetFile, Logger, bStripAll);
		}

		/// <summary>
		/// Replaces a shared object file within an apk, copying the resulting apk to a requested destination
		/// </summary>
		/// <param name="SourceApk">Base apk</param>
		/// <param name="TargetApk">Output location</param>
		/// <param name="SOFile">The so file to replace</param>
		/// <param name="RelativeSOFile">Path of the new SO file, relative to the apk</param>
		/// <param name="Logger">Logger</param>
		public static void ReplaceSO(FileReference SourceApk, FileReference TargetApk, FileReference SOFile, string RelativeSOFile, ILogger Logger)
		{
			UEDeployAndroid.CopyAPKAndReplaceSO(SourceApk, TargetApk, SOFile, RelativeSOFile, Logger);
		}

		/// <summary>
		/// Signs the source APK for debug/development usage
		/// </summary>
		/// <param name="ProjectFile">UE Project file</param>
		/// <param name="SourceApkFile">APK to sign</param>
		/// <param name="TargetApkFile">Location the signed APK is output to</param>
		/// <param name="Logger">Logger for output</param>
		/// <param name="bZipAlign">Whether or not to align the zip before signing</param>
		public static void SignDebugApk(FileReference ProjectFile, FileReference SourceApkFile, FileReference TargetApkFile, ILogger Logger, bool bZipAlign = false)
		{
			if (ProjectFile == null || !FileReference.Exists(ProjectFile))
			{
				throw new BuildException("Failed to sign apk. Project file reference is null or it does not exist");
			}
			if (SourceApkFile == null || !FileReference.Exists(SourceApkFile))
			{
				throw new BuildException("Failed to sign apk. Source file reference is null or it does not exist");
			}
			if (TargetApkFile == null)
			{
				throw new BuildException("Failed to sign apk. Target file reference is null");
			}

			GetToolChain(ProjectFile, Logger).SignApk(SourceApkFile, TargetApkFile, bZipAlign);
		}

		/// <summary>
		/// Obtains the version of Android build tools
		/// </summary>
		/// <param name="ProjectFile">UE project file</param>
		/// <param name="Logger">Logger for output</param>
		/// <returns>Version of Android build tools</returns>
		public static string GetBuildToolsVersion(FileReference ProjectFile, ILogger Logger)
		{
			return GetToolChain(ProjectFile, Logger).GetBuildToolsVersion();
		}

		/// <summary>
		/// Obtains the path of Android build tools
		/// </summary>
		/// <param name="ProjectFile">UE project file</param>
		/// <param name="Logger">Logger for output</param>
		/// <returns>Path to Android build tools</returns>
		public static string GetBuildToolsPath(FileReference ProjectFile, ILogger Logger)
		{
			return GetToolChain(ProjectFile, Logger).GetBuildToolsPath();
		}

		/// <summary>
		///
		/// </summary>
		/// <param name="Target"></param>
		/// <param name="Logger"></param>
		public static string GetAFSExecutable(UnrealTargetPlatform Target, ILogger Logger)
		{
			return UEDeployAndroid.GetAFSExecutable(Target, Logger);
		}
	}
}
