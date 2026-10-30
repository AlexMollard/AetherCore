'use strict';
importScripts('AetherSceneParser.js');
self.onmessage = async ({data}) => {
    if (data.type !== 'prepare') return;
    try {
        const module = await createAetherSceneParser({locateFile: name => new URL(name, self.location.href).href});
        const text = value => {
            const size=module.lengthBytesUTF8(value)+1, pointer=module._malloc(size);
            if(!pointer) throw Error('Scene parser string allocation failed');
            module.stringToUTF8(value,pointer,size); return pointer;
        };
        module._stream_parser_reset();
        for (const file of data.files) {
            const path=text(file.path), bytes=new Uint8Array(file.bytes), pointer=module._malloc(Math.max(1,bytes.length));
            if(!pointer) { module._free(path); throw Error('Scene parser input allocation failed'); }
            module.HEAPU8.set(bytes,pointer);
            const ok=module._stream_parser_mount(path,pointer,bytes.length);
            module._free(pointer); module._free(path);
            if(!ok) throw Error('Scene parser rejected document: '+file.path);
        }
        for (const [alias,pathValue] of data.aliases) {
            const aliasPtr=text(alias), path=text(pathValue);
            const ok=module._stream_parser_alias(aliasPtr,path);
            module._free(aliasPtr); module._free(path);
            if(!ok) throw Error('Scene parser rejected asset alias');
        }
        const root=text(data.root), scenes=text(data.scenesDir), prefabs=text(data.prefabsDir);
        const ok=data.snapshot ? module._stream_parser_prepare_snapshot(root,data.maxOutput) : module._stream_parser_prepare(root,scenes,prefabs,data.maxOutput);
        module._free(root); module._free(scenes); module._free(prefabs);
        if(!ok) throw Error(module.UTF8ToString(module._stream_parser_error()));
        const size=module._stream_parser_size(), pointer=module._stream_parser_data();
        if(size>data.maxOutput || !pointer) throw Error('Scene parser output exceeds admitted budget');
        const result=module.HEAPU8.slice(pointer,pointer+size);
        const dependencySize=data.snapshot?module._stream_parser_dependencies_size():0;
        if(dependencySize>1024*1024) throw Error('Scene dependency output exceeds limit');
        const dependencies=dependencySize ? module.HEAPU8.slice(module._stream_parser_dependencies_data(),module._stream_parser_dependencies_data()+dependencySize) : new Uint8Array();
        module._stream_parser_reset();
        self.postMessage({bytes:result.buffer,dependencies:dependencies.buffer},[result.buffer,dependencies.buffer]);
    } catch(error) { self.postMessage({error:String(error?.message||error)}); }
};
