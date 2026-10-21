import {dotnet} from './_framework/dotnet.js';

try {
    const shaders = await Promise.all(['sprite', 'mesh'].map(async name => {
        const response = await fetch(new URL(name + '.wgsl', import.meta.url));
        if (!response.ok) throw new Error('Browser ' + name + ' shader could not be loaded');
        return [name, new Uint8Array(await response.arrayBuffer())];
    }));
    const runtime = await dotnet.withModuleConfig({
        canvas:document.getElementById('canvas'),
        preRun:[module => {
            module.FS.mkdirTree('/assets');
            for (const [name, shader] of shaders) module.FS.writeFile('/assets/' + name + '.wgsl', shader);
        }]
    }).create();
    await runtime.runMain();
} catch (error) {
    aetherRuntime.status = 'failed';
    aetherRuntime.diagnostics.push({code:'BROWSER_CSHARP_STARTUP',message:String(error)});
}
