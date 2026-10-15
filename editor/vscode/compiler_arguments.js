'use strict';

function compilerArguments(entryPath, schemaPath, extensionPaths, resourceRoot = '') {
    const arguments_ = [];
    for (const directory of extensionPaths) arguments_.push('--extension-path', directory);
    if (resourceRoot) arguments_.push('--resource-root', resourceRoot);
    arguments_.push('--check-module-json', entryPath);
    if (schemaPath) arguments_.push(schemaPath);
    return arguments_;
}

module.exports = { compilerArguments };
