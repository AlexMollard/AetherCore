import {dotnet} from './_framework/dotnet.js';

try {
    const response = await fetch(new URL('sprite.wgsl', import.meta.url));
    if (!response.ok) throw new Error('Browser sprite shader could not be loaded');
    const shader = new Uint8Array(await response.arrayBuffer());
    const runtime = await dotnet.withModuleConfig({
        canvas:document.getElementById('canvas'),
        preRun:[module => { module.FS.mkdirTree('/assets'); module.FS.writeFile('/assets/sprite.wgsl',shader); }]
    }).create();
    await runtime.runMain();
} catch (error) {
    aetherRuntime.status = 'failed';
    aetherRuntime.diagnostics.push({code:'BROWSER_CSHARP_STARTUP',message:String(error)});
}
