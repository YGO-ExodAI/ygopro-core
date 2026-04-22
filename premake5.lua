local ocgcore_config=function()
	-- ExodAI Phase P1 Primitive 1: state serialization. The .proto file is
	-- the committed source; the .pb.cc/.pb.h are generated at build time
	-- (not committed; .gitignored). A PreBuildEvent below invokes protoc
	-- from vcpkg's installed tree so the gencode matches vcpkg's runtime
	-- exactly — avoiding protobuf's strict `PROTOBUF_VERSION != N` check.
	-- See phase_p1_primitive_1_plan.md §1.3 for the rationale.
	--
	-- The .pb.cc/.pb.h are listed explicitly (not via glob) so MSBuild
	-- treats them as project sources even before the first build has run
	-- protoc. Fresh checkouts on Linux/mac builds (if we ever enable them)
	-- would either need `make proto` first or an analogous prebuild hook.
	-- Chunk 8: serialize/*.cpp (save_state, load_state, lua_callback,
	-- refuse_detect, bytecode_cache) added so EDOPro's ocgcore.lib
	-- contains the OCG_DuelSaveState/OCG_DuelLoadState implementation
	-- bodies. Pre-Chunk-8 the vcxproj only had the proto-generated
	-- files, so the C API entry points were declared but unimplemented
	-- for the EDOPro client side. ygoenv's xmake build path was
	-- unaffected (its own xmake.lua includes serialize/*.cpp).
	files { "*.h", "*.hpp", "*.cpp", "RNG/*.hpp", "RNG/*.cpp",
		"serialize/*.h",
		"serialize/*.cpp",
		"serialize/ocg_state.proto",
		"serialize/ocg_state.pb.h",
		"serialize/ocg_state.pb.cc" }
	warnings "Extra"
	cppdialect "C++17"
	rtti "Off"

	filter "configurations:Release"
		optimize "Speed"
	filter "configurations:Debug"
		optimize "Off"
	filter "action:not vs*"
		buildoptions { "-Wno-unused-parameter", "-pedantic" }
	-- Suppress warnings from protobuf-generated .pb.cc sources. Generated
	-- code trips -Wpedantic (e.g., extra semicolons, unused parameters); we
	-- don't own it, so silence rather than fix.
	filter { "action:not vs*", "files:serialize/*.pb.cc" }
		buildoptions { "-Wno-pedantic", "-Wno-unused-parameter", "-Wno-extra" }
	filter { "action:vs*", "files:serialize/*.pb.cc" }
		-- protobuf gencode trips MSVC C4100 (unreferenced formal parameter),
		-- C4127 (conditional expression is constant), and a handful of
		-- C5xxx warnings on newer compilers. We don't own this code.
		disablewarnings { "4100", "4127", "4244", "4267", "4996", "5054" }
	filter { "system:linux" }
		linkoptions { "-Wl,--no-undefined" }
	filter { "system:macosx", "files:processor_visit.cpp" }
		buildoptions { "-fno-exceptions" }
	filter {}

	-- Regenerate .pb.cc/.pb.h from .proto before each compile. vcpkg's
	-- protoc lives under the triplet's tools/protobuf/ subdir. $(VcpkgRoot)
	-- is set by vcpkg.props (imported via the user-level MSBuild
	-- integration) and resolves WITHOUT a trailing slash — hence the
	-- explicit `\installed\` separator below. If the vcpkg integration
	-- drifts, hardcode to the edopro-vcpkg path here. The regen is cheap
	-- (empty schema today; a few ms even at full size) and keeps the
	-- gencode in lockstep with the runtime on every build.
	filter "action:vs*"
		prebuildcommands {
			'"$(VcpkgRoot)\\installed\\$(VcpkgTriplet)\\tools\\protobuf\\protoc.exe" --proto_path="$(SolutionDir)..\\ocgcore\\serialize" --cpp_out="$(SolutionDir)..\\ocgcore\\serialize" "$(SolutionDir)..\\ocgcore\\serialize\\ocg_state.proto"'
		}
	filter {}

	-- Protobuf lite runtime pulled in via vcpkg's autolink (VcpkgAutoLink in
	-- the workspace-level MSBuild integration; see ../premake5.lua:342-346).
	-- Explicit `links "protobuf-lite"` would fail MSVC because vcpkg installs
	-- the static lib as `libprotobuf-lite.lib` (lib prefix preserved from
	-- upstream) and premake's `links` names the bare file, producing a
	-- missing-file link error. Other vcpkg-installed deps (curl/fmt/etc.)
	-- work the same way — they don't appear in any `links { }` either.
	--
	-- For Linux / macOS builds (which don't currently ship but may later),
	-- we'd add `"protobuf-lite"` under a `filter "system:not windows"`
	-- clause; the Unix-style linker adds the `lib` prefix automatically.
	links { "lua" }
	includedirs { "lua/src", "serialize" }
end

if not subproject then
	newoption {
		trigger = "oldwindows",
		description = "Use the v141_xp toolset to support windows XP sp3"
	}
	workspace "ocgcore"
	location "build"
	language "C++"
	objdir "obj"
	configurations { "Debug", "Release" }
	symbols "On"
	staticruntime "on"
	startproject "ocgcoreshared"

	filter "system:windows"
		defines { "WIN32", "_WIN32", "NOMINMAX" }
		platforms {"Win32", "x64", "arm", "arm64"}

	filter "platforms:Win32"
		architecture "x86"

	filter "platforms:x64"
		architecture "x64"

	filter "platforms:arm64"
		architecture "ARM64"

	filter "platforms:arm"
		architecture "ARM"

	filter { "action:vs*", "platforms:Win32 or x64" }
		vectorextensions "SSE2"
		if _OPTIONS["oldwindows"] then
			toolset "v141_xp"
		end

	filter "action:vs*"
		flags "MultiProcessorCompile"

	filter "configurations:Debug"
		defines "_DEBUG"
		targetdir "bin/debug"
		runtime "Debug"

	filter "configurations:Release"
		defines "NDEBUG"
		targetdir "bin/release"

	local function set_target_dir(target,arch)
		filter { "system:windows", "configurations:" .. target, "architecture:" .. arch }
			targetdir("bin/" .. arch .. "/" .. target)
	end

	set_target_dir("debug","x64")
	set_target_dir("debug","arm")
	set_target_dir("debug","arm64")

	set_target_dir("release","x64")
	set_target_dir("release","arm")
	set_target_dir("release","arm64")

	filter { "action:not vs*", "system:windows" }
		buildoptions { "-static-libgcc", "-static-libstdc++", "-static" }
		linkoptions { "-static-libgcc", "-static-libstdc++", "-static" }
		defines { "UNICODE", "_UNICODE" }

	filter { "system:linux" }
		linkoptions { "-static-libgcc", "-static-libstdc++" }

	local function disableWinXPWarnings(prj)
		premake.w('<XPDeprecationWarning>false</XPDeprecationWarning>')
	end

	local function vcpkgStaticTriplet202006(prj)
		premake.w('<VcpkgEnabled>false</VcpkgEnabled>')
		premake.w('<VcpkgUseStatic>false</VcpkgUseStatic>')
		premake.w('<VcpkgAutoLink>false</VcpkgAutoLink>')
	end

	require('vstudio')

	premake.override(premake.vstudio.vc2010.elements, "globals", function(base, prj)
		local calls = base(prj)
		table.insertafter(calls, premake.vstudio.vc2010.targetPlatformVersionGlobal, disableWinXPWarnings)
		table.insertafter(calls, premake.vstudio.vc2010.globals, vcpkgStaticTriplet202006)
		return calls
	end)
end

include "./lua/"

project "ocgcore"
	kind "StaticLib"
	ocgcore_config()

project "ocgcoreshared"
	kind "SharedLib"
	flags "NoImportLib"
-- 	filter "configurations:Release"
-- 		flags "LinkTimeOptimization"
-- 	filter {}
	targetname "ocgcore"
	defines "OCGCORE_EXPORT_FUNCTIONS"
	staticruntime "on"
	visibility "Hidden"
	ocgcore_config()
