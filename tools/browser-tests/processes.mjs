import {spawn} from 'node:child_process';

// Keep the live leader until the platform tree operation has completed.
export async function stopTree(child) {
    if (!child || child.exitCode !== null)
	return;
    if (process.platform === 'win32') {
	await new Promise((resolve, reject) => {
	    const killer = spawn('taskkill', [ '/PID', String(child.pid), '/T', '/F' ], {windowsHide : true, stdio : 'ignore'});
	    const timer = setTimeout(() => {
		killer.kill();
		reject(Error('Owned process tree cleanup timed out'));
	    }, 10000);
	    killer.once('error', error => {
		clearTimeout(timer);
		reject(error);
	    });
	    killer.once('exit', code => {
		clearTimeout(timer);
		code === 0 ? resolve() : reject(Error('Owned process tree cleanup failed'));
	    });
	});
    } else {
	try {
	    process.kill(-child.pid, 'SIGKILL');
	} catch (error) {
	    if (error.code !== 'ESRCH')
		throw error;
	}
    }
    if (child.exitCode === null)
	await new Promise((resolve, reject) => {
	    const timer = setTimeout(() => reject(Error('Owned worker did not exit')), 10000);
	    child.once('exit', () => {
		clearTimeout(timer);
		resolve();
	    });
	});
}
