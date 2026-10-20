// Mono 10.0.9's ES module initializer assigns these Emscripten 3 names.
// Emscripten 6 no longer declares them in its generated module scope.
var _scriptDir;
var __dirname;

// Mono waits for Emscripten 3's Module.ready before publishing its own readiness.
// Emscripten 6 returns that promise from the module factory instead.
Module.ready = new Promise((resolve, reject) => {
    const initialized = Module.onRuntimeInitialized;
    const aborted = Module.onAbort;
    Module.onRuntimeInitialized = () => { initialized?.(); resolve(Module); };
    Module.onAbort = reason => { aborted?.(reason); reject(new Error(String(reason))); };
});

// Mono's asynchronous instantiate hook waits on preInit. Emscripten 6 executes
// preInit before installing the exported runtime helpers Mono needs there.
// Defer Mono's hooks until those helpers exist, immediately before instantiation.
const configureMonoStartup = Module.__dotnet_runtime.configureEmscriptenStartup;
Module.__dotnet_runtime.configureEmscriptenStartup = module => {
    configureMonoStartup(module);
    // Only the old worker bootstrap consumes this; this experiment is single-threaded.
    delete module.mainScriptUrlOrBlob;
    const instantiate = module.instantiateWasm;
    const hooks = module.preInit || [];
    module.preInit = [];
    module.instantiateWasm = (imports, receive) => {
        for (const hook of hooks) hook();
        return instantiate(imports, receive);
    };
};
