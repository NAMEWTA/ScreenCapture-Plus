"""Check out original Git blob bytes in new, owned dependency repositories.

This is an explicit source-build profile. It does not rewrite upstream
attributes, normalize blobs or relax the collector's byte-for-byte comparison.
"""
from pathlib import Path
import runpy

ROOT = Path(__file__).resolve().parents[2]
COLLECTOR = runpy.run_path(str(ROOT / 'scripts/create-source-bundle.py'))
git, require, digest = (COLLECTOR[name] for name in ('git', 'require', 'digest'))
ATTRIBUTES = b'* -text -eol -filter -ident -working-tree-encoding\n'
PROFILE = 'original-git-blob-bytes-v1'


def policy_path(source):
    source = Path(source)
    require(source.is_dir() and not source.is_symlink(), 'Original-byte checkout needs a real owned directory')
    administration = source / '.git'
    require(administration.is_dir() and not administration.is_symlink() and
            (administration / 'info').is_dir() and not (administration / 'info').is_symlink(),
            'Original-byte checkout requires its own initialized Git directory')
    require(Path(git(source, 'rev-parse', '--absolute-git-dir').decode().strip()).resolve() == administration.resolve() and
            Path(git(source, 'rev-parse', '--show-toplevel').decode().strip()).resolve() == source.resolve(),
            'Original-byte checkout must not modify another repository or linked worktree')
    return administration / 'info/attributes'


def verify_policy(source):
    path = policy_path(source)
    require(path.is_file() and not path.is_symlink() and path.read_bytes() == ATTRIBUTES,
            'Original Git source-byte checkout policy is missing or changed')
    require(git(source, 'config', '--local', '--get', 'core.autocrlf').strip() == b'false',
            'Original Git source-byte checkout configuration changed')
    return {'profile': PROFILE, 'path': str(path.resolve()), 'size': len(ATTRIBUTES), 'sha256': digest(path),
            'meaning': 'Original Git blobs, including original LF and CRLF; no working-tree content conversion'}


def install_policy(source):
    """Install once, before any first checkout, without touching prior inputs."""
    source = Path(source)
    path = policy_path(source)
    require({entry.name for entry in source.iterdir()} == {'.git'} and
            not git(source, 'ls-files', '--stage', '-z') and
            not git(source, 'for-each-ref', '--format=%(refname)') and
            git(source, 'symbolic-ref', 'HEAD').strip().startswith(b'refs/heads/'),
            'Install the original-byte policy only in a new empty repository before its first checkout')
    require(not path.exists() and not path.is_symlink(), 'An existing Git attributes policy cannot be replaced')
    with path.open('xb') as output:
        output.write(ATTRIBUTES)
    return verify_policy(source)


def retain_policy(source, recorder):
    """Retain the actual policy and implementing code in this real producer."""
    value = verify_policy(source)
    policy = recorder.retain_record(Path(value['path']))
    helper = recorder.retain_record(Path(__file__).resolve())
    require({key: policy[key] for key in ('size', 'sha256')} ==
            {key: value[key] for key in ('size', 'sha256')} and verify_policy(source) == value,
            'Original source-byte checkout policy changed while being retained')
    return {**value, 'retainedPolicy': policy, 'retainedHelper': helper}
