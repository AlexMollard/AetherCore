import assert from 'node:assert/strict';
import {existsSync} from 'node:fs';
import {test} from 'node:test';

test('required missing checks and unavailable hardware never pass', async () => {
    assert.ok(existsSync(new URL('./report.mjs', import.meta.url)), 'report qualification policy is missing');
    const {qualify} = await import('./report.mjs');
    assert.equal(qualify([ 'a' ], []), 'blocked');
    assert.equal(qualify([ 'a' ], [ {name : 'a', status : 'skipped'} ]), 'blocked');
    assert.equal(qualify([ 'a' ], [ {name : 'a', status : 'passed', hardware : false} ]), 'blocked');
    assert.equal(qualify([ 'a' ], [ {name : 'a', status : 'passed', hardware : true} ]), 'passed');
    assert.equal(qualify([ 'a' ], [ {name : 'a', status : 'passed', hardware : true}, {name : 'a', status : 'passed', hardware : true} ]),
		 'failed');
    assert.equal(qualify([], []), 'blocked');
});
