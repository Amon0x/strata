'use strict';

const assert = require('assert');
const { suggestedReplacement } = require('./quick_fix');

// The misspelled argument name is replaced, not the value or the widget name.
assert.deepStrictEqual(
    suggestedReplacement(
        "Argument 'lable' is not accepted by 'Button'. Did you mean 'label'?",
        'lable: "Save"'
    ),
    { offset: 0, length: 5, replacement: 'label' }
);
// An enum literal is fixed inside its quotes.
assert.deepStrictEqual(
    suggestedReplacement(
        "Expected one of (COLUMN, ROW), but found string literal 'COLUM'. Did you mean 'COLUMN'?",
        '"COLUM"'
    ),
    { offset: 1, length: 5, replacement: 'COLUMN' }
);
// A dotted name only has its own segment replaced.
assert.deepStrictEqual(
    suggestedReplacement(
        "Theme token 'surfaceRasied' is not defined. Did you mean 'surfaceRaised'?",
        'theme.surfaceRasied'
    ),
    { offset: 6, length: 13, replacement: 'surfaceRaised' }
);
// Names quoted in the message but absent from the range are ignored.
assert.deepStrictEqual(
    suggestedReplacement(
        "Action 'state.adjust' targets state 'cont', which no state declares. Did you mean 'count'?",
        '"cont"'
    ),
    { offset: 1, length: 4, replacement: 'count' }
);
// Details the editor appends after the first line do not confuse the parse.
assert.deepStrictEqual(
    suggestedReplacement(
        "Style 'Crad' is not declared. Did you mean 'Card'?\nExpected: Card",
        'Crad'
    ),
    { offset: 0, length: 4, replacement: 'Card' }
);
// No suggestion, no fix; and a name that only appears inside a longer word is not matched.
assert.strictEqual(suggestedReplacement("Style 'Crad' is not declared.", 'Crad'), null);
assert.strictEqual(
    suggestedReplacement("Field 'tit' is not part of record. Did you mean 'title'?", 'subtitle'),
    null
);
