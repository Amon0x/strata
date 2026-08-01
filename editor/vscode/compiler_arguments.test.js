'use strict';

const assert = require('assert');
const { compilerArguments } = require('./compiler_arguments');

assert.deepStrictEqual(
    compilerArguments('ui/main.strata', '', []),
    ['--check-module-json', 'ui/main.strata']
);
assert.deepStrictEqual(
    compilerArguments(
        'ui/main.strata',
        'ui/main.schemas.json',
        ['extensions/first', 'extensions/second']
    ),
    [
        '--extension-path', 'extensions/first',
        '--extension-path', 'extensions/second',
        '--check-module-json', 'ui/main.strata',
        'ui/main.schemas.json',
    ]
);
