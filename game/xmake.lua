target("OxCity")
    set_kind("binary")
    set_languages("cxx23")
    set_default(true)

    add_deps("Oxylus")
    -- rcli compiles the engine shaders and cooks the game assets at build time
    add_deps("rcli")

    add_includedirs("src")
    add_files("src/**.cpp")

    -- the renderer loads `Shaders/engine.oxpack` from the app dir, and the manifest that lists the engine's
    -- shaders lives with the editor's assets, so the game has to reach into OxylusEditor/ for it
    add_files(path.join(os.scriptdir(), "../engine/OxylusEditor/Assets/engine.toml"))
    add_rules("ox.compile_shaders", { output_dir = "Assets/Shaders" })

    -- RmlUi documents, stylesheets and fonts are read straight from disk, copied next to the binary
    -- audio is not packed by the cooker, the manifest points at the source .wav under the assets dir
    add_files("assets/UI/**.rml", "assets/UI/**.rcss", "assets/Fonts/**.ttf", "assets/Audio/**.wav")
    add_rules("ox.install_resources", { root_dir = path.join(os.scriptdir(), "assets"), output_dir = "Assets" })

    -- models and sounds go through the engine's cooker into Assets/.cooked + assets.oxmanifest
    add_rules("ox.cook_assets", { root_dir = path.join(os.scriptdir(), "assets"), output_dir = "Assets/.cooked" })
target_end()
