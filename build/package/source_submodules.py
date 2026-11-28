"""Restore the reviewed Qt gitlink graph from the actual pinned parent trees.

Every child commit comes from its parent's original tree, not a moving branch.
Only the reviewed official repository relationships below can use HTTPS. Each
new independent child checkout uses the same original-blob policy as its parent.
"""
from pathlib import Path
import runpy
from urllib.parse import urljoin

ROOT = Path(__file__).resolve().parents[2]
CHECKOUT = runpy.run_path(str(ROOT / 'build/package/source_checkout.py'))
BUNDLE = CHECKOUT['COLLECTOR']
git, require = BUNDLE['git'], BUNDLE['require']
REVIEWED_ORIGINS = {
    'https://github.com/qt/qttools.git': {'https://code.qt.io/playground/qlitehtml.git'},
    'https://code.qt.io/playground/qlitehtml.git': {'https://code.qt.io/qt/qttools-litehtml.git'},
    'https://code.qt.io/qt/qttools-litehtml.git': set(),
}
MAX_DEPTH = 8
MAX_SUBMODULES = 32


def reviewed_origin(parent, declared):
    # Git treats the remote repository as a directory when resolving ./ or ../.
    # Resolve only those standard forms, then require an exact reviewed URL.
    resolved = urljoin(parent + '/', declared) if declared.startswith(('./', '../')) else declared
    aliases = {'https://code.qt.io/cgit/playground/qlitehtml.git': 'https://code.qt.io/playground/qlitehtml.git',
               'https://code.qt.io/cgit/qt/qttools-litehtml.git': 'https://code.qt.io/qt/qttools-litehtml.git'}
    require(aliases.get(resolved, resolved) in REVIEWED_ORIGINS.get(aliases.get(parent, parent), set()),
            'Unreviewed official submodule repository: ' + declared)
    return resolved


def checkout_identity(source, pin):
    CHECKOUT['verify_policy'](source)
    require(git(source, 'rev-parse', 'HEAD').decode().strip() == pin['commit'],
            'Submodule parent checkout differs from its pinned commit')
    require(git(source, 'remote', 'get-url', 'origin').decode().strip() == pin['repository'],
            'Submodule checkout differs from its original repository')
    require(not git(source, 'status', '--porcelain', '--untracked-files=normal').strip(),
            'Submodule source checkout is not clean')


def children(source, pin):
    """Read both gitlinks and URL declarations from the same original commit."""
    checkout_identity(source, pin)
    entries = BUNDLE['git_files'](source, pin['commit'])
    links = {path: sha for mode, kind, sha, path in entries if mode == '160000' and kind == 'commit'}
    modules = [(mode, kind, sha) for mode, kind, sha, path in entries if path == '.gitmodules']
    if not links and not modules:
        return [], None
    require(len(links) <= MAX_SUBMODULES and len(modules) == 1 and modules[0][:2] == ('100644', 'blob'),
            'Source gitlinks require one original regular .gitmodules blob')
    original = git(source, 'cat-file', 'blob', modules[0][2])
    path = source / '.gitmodules'
    require(path.is_file() and not path.is_symlink() and path.read_bytes() == original,
            'Working .gitmodules differs from the pinned original blob')
    declarations = {}
    for item in git(source, 'config', '--no-includes', '--blob', pin['commit'] + ':.gitmodules',
                    '--null', '--list').split(b'\0'):
        if not item:
            continue
        key, separator, value = item.decode('utf-8').partition('\n')
        require(separator and key.startswith('submodule.') and key.rsplit('.', 1)[-1] in ('path', 'url', 'branch'),
                'Unreviewed original .gitmodules setting')
        name, field = key[len('submodule.'):].rsplit('.', 1)
        require(name and field not in declarations.setdefault(name, {}),
                'Duplicate original submodule declaration')
        declarations[name][field] = value
    result, paths = [], set()
    for name, declaration in declarations.items():
        require({'path', 'url'} <= set(declaration), 'Incomplete original submodule declaration')
        relative = BUNDLE['relative'](declaration['path'])
        require(relative in links and relative not in paths, 'Original gitlink and .gitmodules paths differ')
        repository = reviewed_origin(pin['repository'], declaration['url'])
        paths.add(relative)
        # An upstream branch hint is retained as original metadata only. The
        # fetch below always selects the commit in the original parent tree.
        result.append({'name': name, 'path': relative, 'repository': repository,
                       'declaredUrl': declaration['url'], 'declaredBranch': declaration.get('branch'),
                       'commit': links[relative]})
    require(paths == set(links), 'Original gitlink lacks its exact .gitmodules declaration')
    return sorted(result, key=lambda item: item['path']), modules[0][2]


def owned_child(source, relative):
    child = source / BUNDLE['relative'](relative)
    require(all(not (source / parent).is_symlink() for parent in Path(relative).parents) and
            not child.is_symlink() and child.resolve().is_relative_to(source.resolve()),
            'Source gitlink traverses a directory link')
    require(child.parent.is_dir(), 'Source gitlink parent directory is missing')
    require(not child.exists() or child.is_dir() and not any(child.iterdir()),
            'Restore gitlinks only into new empty directories; preserve earlier inputs')
    child.mkdir(exist_ok=True)
    return child


def restore(source, pin, recorder, environment):
    """Fetch only fixed actual gitlinks and keep the original restoration evidence."""
    source = Path(source)
    recorder.retain_record(Path(__file__).resolve())
    count = 0

    def visit(directory, identity, ancestors):
        nonlocal count
        require(len(ancestors) < MAX_DEPTH and (identity['repository'], identity['commit']) not in ancestors,
                'Cyclic or oversized original submodule graph')
        selected, modules_blob = children(directory, identity)
        tree = git(directory, 'rev-parse', identity['commit'] + '^{tree}').decode().strip()
        result = {**identity, 'tree': tree, 'gitmodulesBlob': modules_blob, 'submodules': []}
        for kind, object_id in (('commit', identity['commit']), ('tree', tree)):
            recorder.run(BUNDLE['git_command'](directory, 'cat-file', kind, object_id),
                         cwd=directory, env=environment)
        recorder.run(BUNDLE['git_command'](directory, 'ls-tree', '-r', '-z', '--full-tree', identity['commit']),
                     cwd=directory, env=environment)
        if modules_blob:
            recorder.retain_record(directory / '.gitmodules')
        for entry in selected:
            count += 1
            require(count <= MAX_SUBMODULES, 'Oversized original submodule graph')
            child = owned_child(directory, entry['path'])
            for arguments in (('init', '--quiet'), ('config', 'core.autocrlf', 'false')):
                recorder.run(BUNDLE['git_command'](child, *arguments), cwd=child, env=environment)
            CHECKOUT['install_policy'](child)
            CHECKOUT['retain_policy'](child, recorder)
            for arguments in (('remote', 'add', 'origin', entry['repository']),
                              ('-c', 'protocol.https.allow=always', 'fetch', '--depth=1', '--no-tags',
                               'origin', entry['commit']),
                              ('checkout', '--detach', 'FETCH_HEAD')):
                recorder.run(BUNDLE['git_command'](child, *arguments), cwd=child, env=environment)
            result['submodules'].append(visit(child, entry, [*ancestors, (identity['repository'], identity['commit'])]))
        checkout_identity(directory, identity)
        return result

    result = visit(source, pin, [])
    output = recorder.directory / (source.name + '-submodules.json')
    with output.open('xb') as stream:
        stream.write(BUNDLE['canonical'](result))
    recorder.retain_record(output)
    return result


def verify(source, pin):
    """Recheck every originally declared checkout and its raw-byte policy."""
    def visit(directory, identity, depth):
        require(depth < MAX_DEPTH, 'Oversized original submodule graph')
        selected, _ = children(directory, identity)
        for entry in selected:
            visit(directory / entry['path'], entry, depth + 1)
    visit(Path(source), pin, 0)
