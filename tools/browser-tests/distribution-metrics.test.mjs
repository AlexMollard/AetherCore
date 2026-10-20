import assert from 'node:assert/strict';
import test from 'node:test';
import {checkBudgets, percentile} from './distribution-metrics.mjs';
test('p95 uses a sorted nearest rank sample without mutating it',()=>{const a=[100,1,2,3];assert.equal(percentile(a,.95),100);assert.deepEqual(a,[100,1,2,3]);assert.throws(()=>percentile([],.95));});
test('budget checks reject missing, nonfinite and oversized required metrics',()=>{
 const budgets={zipBytes:10,coldInteractiveMs:20,warmInteractiveMs:15,wasmLinearMemoryBytes:100,frameIntervalP95Ms:30,editToReadyMs:1000};
 assert.equal(checkBudgets({...budgets},budgets).status,'passed');
 for(const key of Object.keys(budgets)){for(const value of [undefined,NaN,-1,budgets[key]+1]){assert.equal(checkBudgets({...budgets,[key]:value},budgets).status,'failed');}}
 assert.equal(checkBudgets(budgets,{}).status,'failed');
});
