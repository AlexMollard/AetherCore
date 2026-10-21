import {readFileSync} from 'node:fs';

export const {legacyCaseNames, requiredBridgeCases} = JSON.parse(readFileSync(new URL('./required-cases.json', import.meta.url), 'utf8'));

export function qualify(required, cases) {
    if (!required.length)
	return 'blocked';
    if (cases.some(item => item.status === 'failed'))
	return 'failed';
    for (const name of required) {
	const found = cases.filter(item => item.name === name);
	if (found.length > 1)
	    return 'failed';
	if (found.length !== 1 || found[0].status !== 'passed' || found[0].hardware === false)
	    return 'blocked';
    }
    return 'passed';
}
