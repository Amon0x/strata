'use strict';

const vscode = require('vscode');
const fs = require('fs');
const path = require('path');
const childProcess = require('child_process');

const selector = { language: 'strata', scheme: 'file' };
const core = require('./strata-completions.json');
const schemaCache = new Map();
const extensionSchemaCache = new Map();
const validatedUris = new Map();
let discoveredSchemas = [];
let missingCompilerReported = false;

function workspaceFolder(document) {
    const folder = vscode.workspace.getWorkspaceFolder(document.uri);
    return folder ? folder.uri.fsPath : path.dirname(document.uri.fsPath);
}

function resolveWorkspacePath(document, configured) {
    const root = workspaceFolder(document);
    const expanded = configured.replaceAll('${workspaceFolder}', root);
    return path.isAbsolute(expanded) ? expanded : path.resolve(root, expanded);
}

function configuredPath(document, setting) {
    const configured = vscode.workspace.getConfiguration('strata', document.uri).get(setting, '');
    return configured ? resolveWorkspacePath(document, configured) : '';
}

function extensionPaths(document) {
    return vscode.workspace.getConfiguration('strata', document.uri)
        .get('extensions.paths', [])
        .filter(value => typeof value === 'string' && value.length !== 0)
        .map(value => resolveWorkspacePath(document, value));
}

function existing(candidates) {
    for (const candidate of candidates) {
        if (candidate && fs.existsSync(candidate)) return candidate;
    }
    return '';
}

function compilerPath(document) {
    const configured = configuredPath(document, 'compiler.path');
    if (configured) return configured;
    const root = workspaceFolder(document);
    const executable = process.platform === 'win32' ? 'strata_compile.exe' : 'strata_compile';
    return existing([
        process.env.STRATA_COMPILER,
        path.join(root, 'build', 'install', process.platform === 'win32' ? 'windows-x64' : 'linux-x64', 'bin', executable),
        path.join(root, 'build', 'cmake', 'linux-x64', 'native', executable),
        path.join(root, 'build', 'cmake', 'windows-x64', 'native', 'RelWithDebInfo', executable),
    ]) || executable;
}

function registryPath(document) {
    const configured = configuredPath(document, 'registry.path');
    if (configured) return configured;
    const root = workspaceFolder(document);
    return existing([
        process.env.STRATA_REGISTRY,
        path.join(root, 'src', 'main', 'resources', 'strata', 'registry-v1.json'),
        path.join(root, 'share', 'strata', 'registry-v1.json'),
        path.join(root, 'build', 'install', 'linux-x64', 'share', 'strata', 'registry-v1.json'),
        path.join(root, 'build', 'install', 'windows-x64', 'share', 'strata', 'registry-v1.json'),
    ]);
}

function readSchema(file) {
    if (!file || !fs.existsSync(file)) return undefined;
    const modified = fs.statSync(file).mtimeMs;
    const cached = schemaCache.get(file);
    if (cached && cached.modified === modified) return cached.value;
    try {
        const value = JSON.parse(fs.readFileSync(file, 'utf8'));
        schemaCache.set(file, { modified, value });
        return value;
    } catch (_) {
        return undefined;
    }
}

function schemaPath(document) {
    const configured = configuredPath(document, 'applicationSchemas.path');
    if (configured) return configured;
    const adjacent = document.uri.fsPath.replace(/\.strata$/i, '.schemas.json');
    if (fs.existsSync(adjacent)) return adjacent;
    const directory = path.dirname(document.uri.fsPath);
    const nearest = discoveredSchemas.find(candidate => path.dirname(candidate) === directory);
    if (nearest) return nearest;
    return discoveredSchemas.length === 1 ? discoveredSchemas[0] : '';
}

function appSchema(document) {
    return readSchema(schemaPath(document));
}

function schemaDocuments(document) {
    const application = appSchema(document);
    return [
        ...(application ? [application] : []),
        ...(extensionSchemaCache.get(document.uri.toString()) || []),
    ];
}

function typeLabel(type) {
    if (!type || typeof type !== 'object') return 'value';
    if (type.ref) return type.ref;
    if (type.label) return type.label;
    if (type.kind === 'list') return `list of ${typeLabel(type.element)}`;
    if (type.kind === 'map') return `map of ${typeLabel(type.value)}`;
    if (type.kind === 'union') return (type.options || []).map(typeLabel).join(' | ');
    if (type.kind === 'enum') return (type.values || []).join(' | ');
    if (type.kind === 'async') return `async ${typeLabel(type.value)}`;
    return type.kind || 'value';
}

function normalizeParameter(parameter) {
    return {
        name: parameter.name,
        type: typeof parameter.type === 'string' ? parameter.type : typeLabel(parameter.type),
        required: Boolean(parameter.required),
        nullable: Boolean(parameter.nullable),
        aliases: parameter.aliases || [],
        enumValues: parameter.enumValues || (parameter.type && parameter.type.values) || [],
    };
}

function widgets(document) {
    const values = core.widgets.map(widget => ({
        ...widget,
        parameters: widget.parameters.map(normalizeParameter),
    }));
    for (const schema of schemaDocuments(document)) {
        for (const widget of schema.widgets?.definitions || []) {
            values.push({
                ...widget,
                parameters: (widget.parameters || []).map(normalizeParameter),
            });
        }
    }
    return values;
}

function behaviors(document) {
    const values = [...core.behaviors];
    for (const schema of schemaDocuments(document)) {
        for (const behavior of schema.behaviors?.definitions || []) {
            values.push({
                id: behavior.id,
                optionsType: typeLabel(behavior.optionsType || behavior.options),
            });
        }
        for (const id of schema.behaviors?.ids || []) {
            values.push({ id, optionsType: 'map of any' });
        }
    }
    return values;
}

function actions(document) {
    const values = core.actions.map(action => ({ ...action, parameters: action.parameters.map(normalizeParameter) }));
    for (const schema of schemaDocuments(document)) {
        for (const action of schema.actions?.definitions || []) {
            values.push({
                id: action.id,
                summary: action.summary || action.id,
                dispatchPolicy: action.dispatchPolicy || 'optional',
                payloadContract: action.payloadContract || 'no payload',
                parameters: (action.arguments || []).map(normalizeParameter),
            });
        }
    }
    return values;
}

function hostRoots(document) {
    const result = new Map();
    for (const schema of schemaDocuments(document)) {
        for (const root of schema.host || []) result.set(root.path, root.type);
    }
    return result;
}

function objectFields(type) {
    if (!type || typeof type !== 'object') return [];
    if (type.kind === 'object' || type.kind === 'hostObject') return type.fields || [];
    const nonNull = type.kind === 'union'
        ? (type.options || []).find(option => option.kind !== 'null')
        : undefined;
    return nonNull ? objectFields(nonNull) : [];
}

function parameterDocumentation(parameter) {
    const flags = [parameter.required ? 'required' : 'optional'];
    if (parameter.nullable) flags.push('nullable');
    const aliases = parameter.aliases.length ? ` Aliases: ${parameter.aliases.join(', ')}.` : '';
    const values = parameter.enumValues.length ? ` Values: ${parameter.enumValues.join(', ')}.` : '';
    return `**${parameter.name}** — ${parameter.type} (${flags.join(', ')}).${aliases}${values}`;
}

function parameterItems(parameters) {
    return parameters.map(parameter => {
        const item = new vscode.CompletionItem(parameter.name, vscode.CompletionItemKind.Property);
        item.insertText = new vscode.SnippetString(`${parameter.name}: \${1}`);
        item.detail = parameter.type;
        item.documentation = new vscode.MarkdownString(parameterDocumentation(parameter));
        if (!parameter.required) item.sortText = `z-${parameter.name}`;
        return item;
    });
}

function completionProvider() {
    return {
        provideCompletionItems(document, position) {
            const prefix = document.lineAt(position.line).text.slice(0, position.character);
            const actionString = prefix.match(/action\s*\(\s*"([^"]*)$/);
            if (actionString) {
                return actions(document).map(action => {
                    const item = new vscode.CompletionItem(action.id, vscode.CompletionItemKind.Event);
                    item.detail = `${action.dispatchPolicy} · ${action.payloadContract}`;
                    item.documentation = new vscode.MarkdownString(action.summary || action.id);
                    item.filterText = action.id;
                    return item;
                });
            }

            if (/\bid\s*:\s*"[^"]*$/.test(prefix)) {
                return behaviors(document).map(behavior => {
                    const item = new vscode.CompletionItem(behavior.id, vscode.CompletionItemKind.EnumMember);
                    item.detail = `behavior options: ${behavior.optionsType}`;
                    return item;
                });
            }

            const actionCall = prefix.match(/action\s*\(\s*"([^"]+)"[^)]*$/);
            if (actionCall) {
                const action = actions(document).find(candidate => candidate.id === actionCall[1]);
                if (action) return parameterItems(action.parameters);
            }

            const widgetCall = prefix.match(/\b([A-Z][A-Za-z0-9_]*)\s*\([^()]*$/);
            if (widgetCall) {
                const widget = widgets(document).find(candidate => candidate.name === widgetCall[1]);
                if (widget) return parameterItems(widget.parameters);
            }

            const member = prefix.match(/\b([A-Za-z_][A-Za-z0-9_]*)\.([A-Za-z0-9_]*)$/);
            if (member) {
                const type = hostRoots(document).get(member[1]);
                if (type) {
                    return objectFields(type).map(field => {
                        const item = new vscode.CompletionItem(field.name, vscode.CompletionItemKind.Field);
                        item.detail = typeLabel(field.type);
                        item.documentation = new vscode.MarkdownString(
                            `${field.required ? 'Required' : 'Optional'} host field from the application schema.`
                        );
                        return item;
                    });
                }
            }

            const result = [];
            for (const widget of widgets(document)) {
                const item = new vscode.CompletionItem(widget.name, vscode.CompletionItemKind.Class);
                const parameters = widget.parameters;
                const required = parameters.filter(parameter => parameter.required);
                const argumentsSnippet = required.map((parameter, index) =>
                    `${parameter.name}: \${${index + 1}}`
                ).join(', ');
                const body = widget.allowsChildren ? ' {\n\t$0\n}' : '';
                item.insertText = new vscode.SnippetString(`${widget.name}(${argumentsSnippet})${body}`);
                item.detail = `${parameters.length} parameters${widget.allowsChildren ? ' · children' : ''}`;
                item.documentation = new vscode.MarkdownString(
                    parameters.slice(0, 12).map(parameterDocumentation).join('\n\n')
                );
                result.push(item);
            }
            for (const property of [...core.styleProperties, ...core.layoutProperties,
                ...core.animationProperties, ...core.animationTimingProperties]) {
                const item = new vscode.CompletionItem(property.name, vscode.CompletionItemKind.Property);
                item.insertText = new vscode.SnippetString(`${property.name}: \${1};`);
                item.detail = property.type;
                result.push(item);
            }
            for (const helper of core.helpers) {
                const item = new vscode.CompletionItem(helper.name, vscode.CompletionItemKind.Function);
                item.insertText = new vscode.SnippetString(`${helper.name}(\${1})`);
                item.detail = `returns ${helper.returnType}`;
                result.push(item);
            }
            for (const [root, type] of hostRoots(document)) {
                const item = new vscode.CompletionItem(root, vscode.CompletionItemKind.Variable);
                item.detail = `host ${typeLabel(type)}`;
                result.push(item);
            }
            const declarations = document.getText().matchAll(/\b(component|style)\s+([A-Za-z_][A-Za-z0-9_]*)/g);
            for (const declaration of declarations) {
                const item = new vscode.CompletionItem(
                    declaration[2],
                    declaration[1] === 'component' ? vscode.CompletionItemKind.Class : vscode.CompletionItemKind.Struct
                );
                item.detail = `local ${declaration[1]}`;
                result.push(item);
            }
            for (const keyword of ['import', 'screen', 'overlay', 'component', 'style', 'state', 'derived', 'if', 'else', 'when', 'for', 'where', 'root', 'defaults']) {
                result.push(new vscode.CompletionItem(keyword, vscode.CompletionItemKind.Keyword));
            }
            return result;
        },
    };
}

function hoverProvider() {
    return {
        provideHover(document, position) {
            const range = document.getWordRangeAtPosition(position, /[A-Za-z_][A-Za-z0-9_.:-]*/);
            if (!range) return undefined;
            const word = document.getText(range);
            const widget = widgets(document).find(candidate => candidate.name === word);
            if (widget) {
                const markdown = new vscode.MarkdownString(`### ${widget.name}\n\n`);
                markdown.appendMarkdown(widget.parameters.map(parameterDocumentation).join('\n\n'));
                return new vscode.Hover(markdown, range);
            }
            const action = actions(document).find(candidate => candidate.id === word);
            if (action) {
                const markdown = new vscode.MarkdownString(`### ${action.id}\n\n${action.summary || ''}\n\n`);
                markdown.appendMarkdown(`Payload: \`${action.payloadContract}\` · Dispatch: \`${action.dispatchPolicy}\`\n\n`);
                markdown.appendMarkdown(action.parameters.map(parameterDocumentation).join('\n\n'));
                return new vscode.Hover(markdown, range);
            }
            const property = [...core.styleProperties, ...core.layoutProperties,
                ...core.animationProperties, ...core.animationTimingProperties]
                .find(candidate => candidate.name === word);
            if (property) {
                return new vscode.Hover(new vscode.MarkdownString(`**${property.name}** — ${property.type}`), range);
            }
            const helper = core.helpers.find(candidate => candidate.name === word);
            if (helper) {
                return new vscode.Hover(
                    new vscode.MarkdownString(`**${helper.name}** → ${helper.returnType}`), range
                );
            }
            return undefined;
        },
    };
}

function signatureProvider() {
    return {
        provideSignatureHelp(document, position) {
            const prefix = document.getText(new vscode.Range(new vscode.Position(Math.max(0, position.line - 20), 0), position));
            let name;
            let parameters;
            const action = prefix.match(/action\s*\(\s*"([^"]+)"[^)]*$/);
            if (action) {
                const contract = actions(document).find(candidate => candidate.id === action[1]);
                if (!contract) return undefined;
                name = `action("${contract.id}", …)`;
                parameters = contract.parameters;
            } else {
                const widget = prefix.match(/\b([A-Z][A-Za-z0-9_]*)\s*\([^()]*$/);
                const contract = widget && widgets(document).find(candidate => candidate.name === widget[1]);
                if (!contract) return undefined;
                name = `${contract.name}(…)`;
                parameters = contract.parameters;
            }
            const information = new vscode.SignatureInformation(name);
            information.parameters = parameters.map(parameter =>
                new vscode.ParameterInformation(parameter.name, parameterDocumentation(parameter))
            );
            const help = new vscode.SignatureHelp();
            help.signatures = [information];
            help.activeSignature = 0;
            const linePrefix = document.lineAt(position.line).text.slice(0, position.character);
            help.activeParameter = Math.min((linePrefix.match(/,/g) || []).length, Math.max(0, parameters.length - 1));
            return help;
        },
    };
}

function symbolProvider() {
    const kinds = {
        screen: vscode.SymbolKind.Namespace,
        overlay: vscode.SymbolKind.Namespace,
        component: vscode.SymbolKind.Class,
        style: vscode.SymbolKind.Struct,
        state: vscode.SymbolKind.Variable,
        derived: vscode.SymbolKind.Property,
    };
    return {
        provideDocumentSymbols(document) {
            const symbols = [];
            const expression = /^\s*(screen|overlay|component|style|state|derived)\s+([A-Za-z_][A-Za-z0-9_]*)/;
            for (let line = 0; line < document.lineCount; ++line) {
                const match = document.lineAt(line).text.match(expression);
                if (!match) continue;
                const start = document.lineAt(line).text.indexOf(match[2]);
                const range = new vscode.Range(line, start, line, start + match[2].length);
                symbols.push(new vscode.DocumentSymbol(match[2], match[1], kinds[match[1]], range, range));
            }
            return symbols;
        },
    };
}

function definitionProvider() {
    return {
        provideDefinition(document, position) {
            const line = document.lineAt(position.line).text;
            const imported = line.match(/\bimport\s+"([^"]+)"/);
            if (imported) {
                const start = line.indexOf(imported[1]);
                if (position.character >= start &&
                    position.character <= start + imported[1].length) {
                    const destination = path.resolve(path.dirname(document.uri.fsPath), imported[1]);
                    if (fs.existsSync(destination)) {
                        return new vscode.Location(vscode.Uri.file(destination), new vscode.Position(0, 0));
                    }
                }
            }
            const range = document.getWordRangeAtPosition(position, /[A-Za-z_][A-Za-z0-9_]*/);
            if (!range) return undefined;
            const name = document.getText(range);
            const declaration = new RegExp(`^\\s*(?:component|style|screen|overlay|state|derived)\\s+${name}\\b`);
            for (let index = 0; index < document.lineCount; ++index) {
                const text = document.lineAt(index).text;
                if (!declaration.test(text)) continue;
                return new vscode.Location(document.uri, new vscode.Position(index, text.indexOf(name)));
            }
            return undefined;
        },
    };
}

function diagnosticSeverity(value) {
    if (value === 'warning') return vscode.DiagnosticSeverity.Warning;
    if (value === 'info') return vscode.DiagnosticSeverity.Information;
    return vscode.DiagnosticSeverity.Error;
}

function compilerRange(range) {
    if (!range) return new vscode.Range(0, 0, 0, 1);
    const startLine = Math.max(0, Number(range.start?.line || 1) - 1);
    const startColumn = Math.max(0, Number(range.start?.column || 1) - 1);
    const endLine = Math.max(startLine, Number(range.end?.line || range.start?.line || 1) - 1);
    let endColumn = Math.max(0, Number(range.end?.column || range.start?.column || 1) - 1);
    if (startLine === endLine && endColumn <= startColumn) endColumn = startColumn + 1;
    return new vscode.Range(startLine, startColumn, endLine, endColumn);
}

function executeCompiler(executable, args, cwd) {
    return new Promise(resolve => {
        childProcess.execFile(executable, args, { cwd, maxBuffer: 16 * 1024 * 1024 },
            (error, stdout, stderr) => resolve({ error, stdout, stderr }));
    });
}

async function validateDocument(document, diagnostics, output, explicit) {
    if (document.languageId !== 'strata' || document.uri.scheme !== 'file') return;
    if (document.isDirty) {
        if (!explicit) return;
        if (!(await document.save())) return;
    }
    const registry = registryPath(document);
    if (!registry) {
        if (explicit) vscode.window.showErrorMessage('Strata registry-v1.json was not found. Configure strata.registry.path.');
        return;
    }
    const compiler = compilerPath(document);
    const schema = schemaPath(document);
    const args = [];
    for (const directory of extensionPaths(document)) args.push('--extension-path', directory);
    args.push('--check-module-json', document.uri.fsPath, registry);
    if (schema) args.push(schema);
    const result = await executeCompiler(compiler, args, workspaceFolder(document));
    const lines = result.stdout.trim().split(/\r?\n/);
    let line = '';
    for (let index = lines.length - 1; index >= 0; --index) {
        if (lines[index].startsWith('{')) {
            line = lines[index];
            break;
        }
    }
    if (!line) {
        output.appendLine(`Validation failed to start ${compiler}: ${result.stderr || result.error || 'no diagnostics output'}`);
        if (explicit || !missingCompilerReported) {
            missingCompilerReported = true;
            vscode.window.showWarningMessage('Strata compiler was not found or produced no diagnostics. Configure strata.compiler.path.');
        }
        return;
    }
    missingCompilerReported = false;
    let report;
    try {
        report = JSON.parse(line);
    } catch (error) {
        output.appendLine(`Invalid Strata compiler diagnostics: ${error}`);
        return;
    }
    const entryKey = document.uri.toString();
    extensionSchemaCache.set(
        entryKey,
        (report.extensionSchemas || []).filter(value => value && typeof value === 'object')
    );
    const grouped = new Map();
    for (const record of report.diagnostics || []) {
        let file = record.range?.sourceId || document.uri.fsPath;
        if (!path.isAbsolute(file)) file = path.resolve(path.dirname(document.uri.fsPath), file);
        const uri = vscode.Uri.file(file);
        const diagnostic = new vscode.Diagnostic(
            compilerRange(record.range), record.message || 'Strata compiler diagnostic', diagnosticSeverity(record.severity)
        );
        diagnostic.code = record.code;
        diagnostic.source = 'strata';
        const details = [record.componentPath && `Component: ${record.componentPath}`,
            record.expected && `Expected: ${record.expected}`].filter(Boolean);
        if (details.length) diagnostic.message += `\n${details.join('\n')}`;
        const key = uri.toString();
        if (!grouped.has(key)) grouped.set(key, { uri, values: [] });
        grouped.get(key).values.push(diagnostic);
    }
    for (const previous of validatedUris.get(entryKey) || []) {
        diagnostics.delete(vscode.Uri.parse(previous));
    }
    diagnostics.set(document.uri, grouped.get(entryKey)?.values || []);
    const published = new Set([entryKey]);
    for (const group of grouped.values()) {
        diagnostics.set(group.uri, group.values);
        published.add(group.uri.toString());
    }
    validatedUris.set(entryKey, published);
    output.appendLine(`${report.succeeded ? 'OK' : 'FAILED'} ${document.uri.fsPath} (${(report.diagnostics || []).length} diagnostics)`);
    if (explicit) output.show(true);
}

async function discoverApplicationSchemas() {
    const files = await vscode.workspace.findFiles('**/*.schemas.json', '**/{build,node_modules,.git}/**', 128);
    discoveredSchemas = files.map(uri => uri.fsPath);
}

function activate(context) {
    const diagnostics = vscode.languages.createDiagnosticCollection('strata');
    const output = vscode.window.createOutputChannel('Strata');
    context.subscriptions.push(diagnostics, output);

    discoverApplicationSchemas();
    context.subscriptions.push(
        vscode.languages.registerCompletionItemProvider(selector, completionProvider(), '.', '"', ':'),
        vscode.languages.registerHoverProvider(selector, hoverProvider()),
        vscode.languages.registerSignatureHelpProvider(selector, signatureProvider(), '(', ','),
        vscode.languages.registerDocumentSymbolProvider(selector, symbolProvider()),
        vscode.languages.registerDefinitionProvider(selector, definitionProvider()),
        vscode.commands.registerCommand('strata.validateModule', async () => {
            const document = vscode.window.activeTextEditor?.document;
            if (document) await validateDocument(document, diagnostics, output, true);
        }),
        vscode.commands.registerCommand('strata.reloadSchema', async () => {
            schemaCache.clear();
            extensionSchemaCache.clear();
            await discoverApplicationSchemas();
            vscode.window.showInformationMessage('Strata schemas and completion metadata reloaded.');
        }),
        vscode.workspace.onDidOpenTextDocument(document => {
            if (vscode.workspace.getConfiguration('strata', document.uri).get('validation.onSave', true)) {
                validateDocument(document, diagnostics, output, false);
            }
        }),
        vscode.workspace.onDidSaveTextDocument(document => {
            schemaCache.clear();
            if (vscode.workspace.getConfiguration('strata', document.uri).get('validation.onSave', true)) {
                validateDocument(document, diagnostics, output, false);
            }
        }),
        vscode.workspace.onDidCloseTextDocument(document => {
            const key = document.uri.toString();
            for (const published of validatedUris.get(key) || []) {
                diagnostics.delete(vscode.Uri.parse(published));
            }
            validatedUris.delete(key);
            extensionSchemaCache.delete(key);
        }),
        vscode.workspace.onDidChangeConfiguration(event => {
            if (!event.affectsConfiguration('strata')) return;
            schemaCache.clear();
            extensionSchemaCache.clear();
            for (const document of vscode.workspace.textDocuments) {
                if (document.languageId === 'strata') validateDocument(document, diagnostics, output, false);
            }
        }),
        vscode.workspace.createFileSystemWatcher('**/*.schemas.json')
    );
    const watcher = context.subscriptions[context.subscriptions.length - 1];
    watcher.onDidCreate(discoverApplicationSchemas);
    watcher.onDidDelete(discoverApplicationSchemas);
    watcher.onDidChange(() => {
        schemaCache.clear();
        discoverApplicationSchemas();
    });

    for (const document of vscode.workspace.textDocuments) {
        if (document.languageId === 'strata' &&
            vscode.workspace.getConfiguration('strata', document.uri).get('validation.onSave', true)) {
            validateDocument(document, diagnostics, output, false);
        }
    }
}

function deactivate() {}

module.exports = { activate, deactivate };
