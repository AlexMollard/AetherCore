import {dotnet} from './_framework/dotnet.js';
try {
    const runtime = await dotnet.withDiagnosticTracing(true).create();
    await runtime.runMain();
} catch (error) {
    console.error('CSHARP_WASM_STARTUP', error);
    throw error;
}
