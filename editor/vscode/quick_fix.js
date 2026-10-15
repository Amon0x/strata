'use strict';

// The compiler ends a diagnostic about a likely misspelling with "Did you mean 'x'?". This finds
// the misspelled name inside the diagnostic's source range, so the editor can offer the
// replacement as a quick fix without a second parser.

function editDistance(left, right) {
    const previous = Array.from({ length: right.length + 1 }, (_, index) => index);
    for (let row = 1; row <= left.length; ++row) {
        let diagonal = previous[0];
        previous[0] = row;
        for (let column = 1; column <= right.length; ++column) {
            const above = previous[column];
            const same = left[row - 1].toLowerCase() === right[column - 1].toLowerCase();
            previous[column] = Math.min(above + 1, previous[column - 1] + 1, diagonal + (same ? 0 : 1));
            diagonal = above;
        }
    }
    return previous[right.length];
}

function escapeRegExp(value) {
    return value.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
}

// Returns { offset, length, replacement } relative to `rangeText`, or null when the message makes
// no suggestion or the misspelled name cannot be found in the range.
function suggestedReplacement(message, rangeText) {
    const firstLine = String(message || '').split(/\r?\n/, 1)[0];
    const suggestion = /Did you mean '([^']+)'\?\s*$/.exec(firstLine);
    if (!suggestion) return null;
    const replacement = suggestion[1];
    const quoted = [...firstLine.slice(0, suggestion.index).matchAll(/'([^']+)'/g)]
        .map(match => match[1])
        .filter(name => name !== replacement);
    let best = null;
    for (const name of quoted) {
        const match = new RegExp(`(?<![\\w-])${escapeRegExp(name)}(?![\\w-])`).exec(rangeText);
        if (!match) continue;
        const distance = editDistance(name, replacement);
        if (best === null || distance < best.distance) {
            best = { offset: match.index, length: name.length, replacement, distance };
        }
    }
    return best && { offset: best.offset, length: best.length, replacement: best.replacement };
}

module.exports = { suggestedReplacement };
