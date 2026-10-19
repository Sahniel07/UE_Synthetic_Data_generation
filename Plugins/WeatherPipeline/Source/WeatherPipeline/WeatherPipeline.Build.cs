using UnrealBuildTool;
using System.IO;
using System.Linq;

public class WeatherPipeline : ModuleRules
{
	public WeatherPipeline(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core", "CoreUObject", "Engine", "InputCore", "EnhancedInput",
			"Niagara",              // MyActor_controllingWeather still includes NiagaraComponent.h;
			                        // weather itself now comes from Ultra Dynamic Weather, not Niagara.
			"Networking", "Sockets" // RTSPStreamer.h is a public header (by-value member of AMyActor_weather)
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"RenderCore", "RHI"     // render-target readback in the capture path
		});

		if (Target.bBuildEditor)
		{
			// GEditor->RequestEndPlayMap() in RequestGameExit() — ends PIE the same way the
			// Stop button does, so World Partition's streaming data is flushed before the next Play.
			PrivateDependencyModuleNames.Add("UnrealEd");
		}

		// --- Optional Cesium integration -------------------------------------------------------
		//
		// The pipeline ships in two forms from one codebase: a portable build that works in any
		// Unreal world, and a Cesium build that calculates the ground height from the tileset
		// (StartGroundHeightCalculation in GeoidModel.cpp).
		//
		// Everything else Cesium-related still goes through reflection and stays available either
		// way. This dependency exists for one thing reflection genuinely cannot reach:
		// ACesium3DTileset::SampleHeightMostDetailed takes a plain (non-dynamic) delegate carrying
		// FCesiumSampleHeightResult, and neither the delegate nor the struct can be bound or
		// declared without the real types.
		//
		// Detected rather than assumed, so the module keeps building when Cesium is absent - which
		// is the portable configuration, not an error.
		// Always defined, to 1 or 0, never left undefined. An undefined macro evaluates to 0 in an
		// #if, so a typo or a missing definition would silently compile the unavailable-stub branch
		// while the build reported success - the failure mode being guarded against here is a
		// feature that is quietly inert, not one that fails to compile.
		//
		// To confirm which branch was actually built, read the generated
		//   Intermediate/Build/.../WeatherPipeline/Definitions.WeatherPipeline.h
		// UBT emits module definitions into that force-included header, not as /D flags on the
		// compile command, so inspecting the .obj.rsp for /DWITH_CESIUM_RUNTIME finds nothing and
		// proves nothing.
		bool bHasCesium = HasCesiumPlugin(Target);

		if (bHasCesium)
		{
			PrivateDependencyModuleNames.Add("CesiumRuntime");
		}

		// Private: only GeoidModel.cpp tests it; without Cesium the ground height falls back to the
		// collision probe.
		PrivateDefinitions.Add("WITH_CESIUM_RUNTIME=" + (bHasCesium ? "1" : "0"));
	}

	/**
	 * True when CesiumForUnreal is installed either in the engine's Marketplace folder or in the
	 * project. Both roots are searched because an engine-wide install and a project-local copy are
	 * both normal, and the Marketplace directory name is mangled per install so it cannot be
	 * hardcoded. The search is scoped to those two roots rather than all of Engine/Plugins, which
	 * would walk many thousands of files on every build.
	 */
	private bool HasCesiumPlugin(ReadOnlyTargetRules Target)
	{
		const string Descriptor = "CesiumForUnreal.uplugin";

		string[] SearchRoots =
		{
			Path.Combine(EngineDirectory, "Plugins", "Marketplace"),
			Target.ProjectFile != null
				? Path.Combine(Target.ProjectFile.Directory.FullName, "Plugins")
				: null
		};

		foreach (string Root in SearchRoots)
		{
			if (string.IsNullOrEmpty(Root) || !Directory.Exists(Root))
			{
				continue;
			}

			if (Directory.EnumerateFiles(Root, Descriptor, SearchOption.AllDirectories).Any())
			{
				return true;
			}
		}

		return false;
	}
}
