/*
 * Custom install steps for the RenoDX DLSS bundle.
 *
 * The setup fetches a manifest from GitHub at install time, so add-on and DLL
 * updates can be published remotely without rebuilding the installer:
 *
 *   [Addon]
 *   PackageName=...
 *   DownloadUrl64=...      ; direct link to a .addon64 (or archive containing one)
 *   DownloadUrl32=...      ; optional 32-bit variant
 *   EffectInstallPath=...  ; optional, defaults to .\reshade-shaders\Shaders
 *
 *   [DLLs]
 *   PackageName=...
 *   DownloadUrl=...        ; zip extracted next to the game executable
 *
 *   [SomeGame.exe]         ; optional per-game overrides, keyed by executable name
 *   AddonDownloadUrl64=...
 *   AddonDownloadUrl32=...
 *   DllsDownloadUrl=...
 *   SkipAddon=1
 *   SkipDlls=1
 */

using System;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Net;
using ReShade.Setup.Pages;
using ReShade.Setup.Utilities;

namespace ReShade.Setup
{
	public partial class MainWindow
	{
		const string ManifestUrl = "https://raw.githubusercontent.com/yumlevi/renodx-dlss-installer/main/manifest.ini";

		// Sub-directories launchers commonly hide the real game executable in
		static readonly string[] BinaryDirCandidates = {
			"bin", "bin\\x64", "bin\\win64", "bin\\x86_64", "bin\\Release",
			"Binaries\\Win64", "binaries", "x64", "win64", "game"
		};

		// Executable names that are definitely not the game itself
		static readonly string[] IgnoredExecutableNames = {
			"crash", "setup", "unins", "launcher", "redist", "easyanticheat", "eac", "config",
			"activation", "touchup", "helper", "report", "diagnostics", "vc_redist", "dxsetup"
		};

		static bool HasGraphicsApiImports(PEInfo peInfo)
		{
			return peInfo.Modules.Any(s =>
				s.StartsWith("d3d", StringComparison.OrdinalIgnoreCase) ||
				s.StartsWith("dxgi", StringComparison.OrdinalIgnoreCase) ||
				s.StartsWith("ddraw", StringComparison.OrdinalIgnoreCase) ||
				s.StartsWith("opengl32", StringComparison.OrdinalIgnoreCase) ||
				s.StartsWith("vulkan-1", StringComparison.OrdinalIgnoreCase) ||
				s.Contains("GFSDK"));
		}

		// In case a launcher or bootstrap executable was selected, try and find the actual game
		// executable in a nearby binary directory (e.g. "bin\x64"). Returns null if the selected
		// executable already looks like the real game (or no better candidate was found).
		static string FindActualGameExecutable(string targetPath)
		{
			try
			{
				if (HasGraphicsApiImports(new PEInfo(targetPath)))
				{
					return null;
				}

				string baseDir = Path.GetDirectoryName(targetPath);

				foreach (string candidateDir in BinaryDirCandidates)
				{
					string dir = Path.Combine(baseDir, candidateDir);
					if (!Directory.Exists(dir))
					{
						continue;
					}

					var candidates = Directory.EnumerateFiles(dir, "*.exe", SearchOption.TopDirectoryOnly)
						.Where(exe => !IgnoredExecutableNames.Any(name => Path.GetFileNameWithoutExtension(exe).IndexOf(name, StringComparison.OrdinalIgnoreCase) >= 0))
						.OrderByDescending(exe => new FileInfo(exe).Length)
						.ToList();

					foreach (string exe in candidates)
					{
						try
						{
							if (HasGraphicsApiImports(new PEInfo(exe)))
							{
								return exe;
							}
						}
						catch (SystemException)
						{
							// Not a readable PE file, ignore
						}
					}

					// Fallback: a single executable that is bigger than the selected one is most likely the game
					if (candidates.Count == 1 && new FileInfo(candidates[0]).Length > new FileInfo(targetPath).Length)
					{
						return candidates[0];
					}
				}
			}
			catch (SystemException)
			{
				// Ignore and stick with the original selection
			}

			return null;
		}

		// Installed after the ReShade module: downloads the manifest and installs the bundled
		// add-on and DLL package, then finishes the setup.
		void InstallStep_InstallCustomPayload()
		{
			UpdateStatus("Downloading package manifest ...");

			IniFile manifest = null;

			using (var client = new WebClient())
			{
				client.CachePolicy = new System.Net.Cache.RequestCachePolicy(System.Net.Cache.RequestCacheLevel.NoCacheNoStore);

				try
				{
					using (Stream manifestStream = client.OpenRead(ManifestUrl))
					{
						manifest = new IniFile(manifestStream);
					}
				}
				catch (WebException ex)
				{
					UpdateStatusAndFinish(false, "ReShade was installed, but downloading the package manifest failed:\n" + ex.Message + "\n\nCheck your internet connection and run this setup again to install the add-on and DLSS files.");
					return;
				}
			}

			string executableName = Path.GetFileName(currentInfo.targetPath);
			string gameSection = manifest.GetSections().FirstOrDefault(s => s.Equals(executableName, StringComparison.OrdinalIgnoreCase));

			// Install the add-on
			if (gameSection == null || manifest.GetString(gameSection, "SkipAddon", "0") != "1")
			{
				string addonUrlKey = currentInfo.is64Bit ? "DownloadUrl64" : "DownloadUrl32";
				string addonUrl = gameSection != null ? manifest.GetString(gameSection, "Addon" + addonUrlKey) : null;
				if (string.IsNullOrEmpty(addonUrl))
				{
					addonUrl = manifest.GetString("Addon", addonUrlKey);
				}

				if (!string.IsNullOrEmpty(addonUrl))
				{
					var addon = new Addon
					{
						Name = manifest.GetString("Addon", "PackageName", "bundled add-on"),
						DownloadUrl = addonUrl,
						EffectInstallPath = manifest.GetString("Addon", "EffectInstallPath", ".\\reshade-shaders\\Shaders")
					};

					InstallStep_DownloadAddon(addon);

					if (currentOperation == InstallOperation.Finished)
					{
						return; // Download or installation failed and the status page already shows the error
					}
				}
			}

			// Install the DLL package
			if (gameSection == null || manifest.GetString(gameSection, "SkipDlls", "0") != "1")
			{
				string dllsUrl = gameSection != null ? manifest.GetString(gameSection, "DllsDownloadUrl") : null;
				if (string.IsNullOrEmpty(dllsUrl))
				{
					dllsUrl = manifest.GetString("DLLs", "DownloadUrl");
				}

				if (!string.IsNullOrEmpty(dllsUrl))
				{
					InstallStep_InstallDllPackage(manifest.GetString("DLLs", "PackageName", "DLL package"), dllsUrl);

					if (currentOperation == InstallOperation.Finished)
					{
						return;
					}
				}
			}

			InstallStep_Finish();
		}

		void InstallStep_InstallDllPackage(string name, string url)
		{
			UpdateStatus("Downloading " + name + " ...");

			string downloadPath = Path.Combine(Path.GetTempPath(), "ReShadeSetupDllPackage.tmp");

			using (var client = new WebClient())
			{
				client.DownloadProgressChanged += (object sender, DownloadProgressChangedEventArgs e) =>
				{
					if (e.TotalBytesToReceive > 0)
					{
						UpdateStatus("Downloading " + name + " ... (" + ((100 * e.BytesReceived) / e.TotalBytesToReceive) + "%)");
					}
				};

				try
				{
					client.DownloadFileTaskAsync(new Uri(url), downloadPath).Wait();
				}
				catch (AggregateException ex)
				{
					UpdateStatusAndFinish(false, "Failed to download from " + url + ":\n" + ex.InnerException.Message);
					return;
				}
			}

			UpdateStatus("Installing " + name + " ...");

			string tempPath = Path.Combine(Path.GetTempPath(), "reshade-dll-package");
			string targetPath = Path.GetDirectoryName(currentInfo.targetPath);

			try
			{
				if (Directory.Exists(tempPath))
				{
					Directory.Delete(tempPath, true);
				}

				ZipFile.ExtractToDirectory(downloadPath, tempPath);

				// If the archive contains just a single root directory, use its contents
				string rootPath = tempPath;
				if (Directory.GetFiles(rootPath).Length == 0)
				{
					string[] rootDirs = Directory.GetDirectories(rootPath);
					if (rootDirs.Length == 1)
					{
						rootPath = rootDirs[0];
					}
				}

				MoveFiles(rootPath, targetPath);

				Directory.Delete(tempPath, true);
				File.Delete(downloadPath);
			}
			catch (SystemException ex)
			{
				UpdateStatusAndFinish(false, "Failed to install " + name + ":\n" + ex.Message + "\n\nMake sure the target application is not still running!");
			}
		}
	}
}
