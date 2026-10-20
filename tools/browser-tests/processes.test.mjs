import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {existsSync} from 'node:fs';
import {test} from 'node:test';

test('cancelling an owned worker closes its live descendant too', async () => {
    assert.ok(existsSync(new URL('./processes.mjs', import.meta.url)), 'owned tree cleanup helper missing');
    const {stopTree} = await import('./processes.mjs');
    const script =
	"const {spawn}=require('node:child_process');const child=spawn(process.execPath,['-e','setInterval(()=>{},1000)'],{windowsHide:true,stdio:'ignore'});console.log(child.pid);setInterval(()=>{},1000)";
    const owned = spawn(process.execPath, [ '-e', script ],
			{windowsHide : true, detached : process.platform !== 'win32', stdio : [ 'ignore', 'pipe', 'ignore' ]});
    const pid = await new Promise(resolve => owned.stdout.once('data', data => resolve(Number(data.toString().trim()))));
    try {
	await stopTree(owned);
	let alive = true;
	for (let i = 0; i < 100; ++i) {
	    try {
		process.kill(pid, 0);
	    } catch {
		alive = false;
		break;
	    }
	    await new Promise(r => setTimeout(r, 20));
	}
	assert.equal(alive, false, 'owned descendant is still running');
    } finally {
	try {
	    process.kill(pid, 'SIGKILL');
	} catch {
	};
	try {
	    owned.kill('SIGKILL');
	} catch {
	}
    }
});
