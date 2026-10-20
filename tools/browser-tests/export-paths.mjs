export function isExportPath(value) {
    return typeof value==='string' && value.length>0 && !/[:\\]/.test(value) &&
        value.split('/').every(part=>part.length>0 && part!=='.' && part!=='..');
}
