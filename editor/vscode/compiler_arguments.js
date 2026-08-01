'use strict';

function compilerArguments(entryPath, schemaPath, extensionPaths) {
    const arguments_ = [];
    for (const directory of extensionPaths) arguments_.push('--extension-path', directory);
    arguments_.push('--check-module-json', entryPath);
    if (schemaPath) arguments_.push(schemaPath);
    return arguments_;
}

module.exports = { compilerArguments };
