#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
#   test_check_dotnet_packages.py <folder with the five .nupkg files> [<commit>]
#
# D10C-5 (BC-17, d10 L15): proves scripts/check_dotnet_packages.sh REFUSES each way a
# package can break the native-code split, each FOR ITS OWN REASON. It drives the check
# from outside, over copies of the packed folder that this script mutates; the folder
# itself is never edited.
#
#   baseline  the unmutated folder passes (a control that cannot pass proves nothing)
#   C1  a runtimes/ entry injected into Eiva.Fletcher            (a guard: refused before
#                                                                and after D10C-5)
#   C2  Eiva.Fletcher.GatewayClient depends on the server package
#   C3  the server package carries a third native file
#   C4  the server package does not depend on Eiva.Fletcher.Interop
#   C5  the server package carries the edge notice (no `arrow` listed)
#   C6  Eiva.Fletcher depends on the server package
#   C7  SERVER_CEILING=1 (the server package is above its ceiling)
#   C8  a package that is not the server package carries a notice listing `arrow`
#   C9  Eiva.Fletcher lacks its Interop dependency in ONE target-framework group
#   C10 an assembly in the server package (only its entry allow-list refuses it)
#   C11 a shim outside its own runtimes/ entries, in a package that is not the server package
#   C12 a dependency on the server package outside every target-framework group
#   C13 a nuspec with no target-framework group at all
#   C14 a shipped server notice that lacks a library one platform's graph lists
#
# A refusal counts only if the check exits non-zero AND one single `::error::` line names
# the package it refused and the thing that is wrong with it. A check that exits 1 for
# some other reason therefore does not pass a control, and a mutation that broke something
# else would show up in the baseline, not here.
#
# Needs bash and python3 on the PATH (on Windows, Git for Windows' bash).
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile

HERE = pathlib.Path(__file__).resolve().parent
CHECK = HERE / 'check_dotnet_packages.sh'
COVERS = HERE / 'check_notice_covers.sh'

BASE = 'Eiva.Fletcher'
INTEROP = 'Eiva.Fletcher.Interop'
SERVER = 'Eiva.Fletcher.Server'
GATEWAY = 'Eiva.Fletcher.GatewayClient'
# The rule list is BC-17, d10 L15; an error for it names one of the two.
CITES = ('BC-17', 'L15')


def package(folder: pathlib.Path, pid: str) -> pathlib.Path:
    found = sorted(folder.glob(f'{pid}.[0-9]*.nupkg'))
    if not found:
        sys.exit(f'{folder} holds no {pid}.<version>.nupkg: this test needs the five packed packages')
    return found[0]


def nuspec_name(path: pathlib.Path) -> str:
    with zipfile.ZipFile(path) as z:
        return next(n for n in z.namelist() if n.endswith('.nuspec'))


def read(path: pathlib.Path, name: str) -> bytes:
    with zipfile.ZipFile(path) as z:
        return z.read(name)


def mutate(path: pathlib.Path, replace=None, add=None) -> None:
    """Rewrite a .nupkg in place: `replace` swaps the bytes of existing entries, `add`
    appends new ones. Everything else is carried over entry for entry."""
    replace, add = replace or {}, add or {}
    tmp = path.with_suffix('.tmp')
    with zipfile.ZipFile(path) as src, zipfile.ZipFile(tmp, 'w') as dst:
        for info in src.infolist():
            data = replace.get(info.filename)
            if data is None:
                data = src.read(info.filename)
            # A fresh ZipInfo: writing one updates it, and `info` still belongs to `src`.
            out = zipfile.ZipInfo(info.filename, info.date_time)
            out.external_attr = info.external_attr
            dst.writestr(out, data, compress_type=info.compress_type)
        for name, data in add.items():
            dst.writestr(name, data, compress_type=zipfile.ZIP_STORED)
    tmp.replace(path)
    for name in replace:
        with zipfile.ZipFile(path) as z:
            assert name in z.namelist(), f'{path.name}: {name} to replace is not there'


def add_dependency(spec: str, dep_id: str) -> str:
    """The dependency in every target-framework group, as NuGet writes one."""
    version = re.search(r'<version>([^<]+)</version>', spec).group(1)
    dep = f'<dependency id="{dep_id}" version="{version}" exclude="Build,Analyzers" />'
    out = re.sub(r'(<group targetFramework="[^"]+")\s*/>', r'\1></group>', spec)
    out = out.replace('</group>', dep + '</group>')
    assert out.count(f'id="{dep_id}"') == out.count('</group>') > 0, 'a dependency in every group'
    return out


def drop_dependency(spec: str, dep_id: str, tfm: str = '') -> str:
    """The dependency out of every group, or out of the one group for `tfm`."""
    dep = rf'\s*<dependency id="{re.escape(dep_id)}"[^>]*/>'
    if tfm:
        group = re.search(rf'<group targetFramework="{re.escape(tfm)}">.*?</group>', spec, re.S).group(0)
        out = spec.replace(group, re.sub(dep, '', group))
    else:
        out = re.sub(dep, '', spec)
    assert out != spec, f'{dep_id} was not a dependency'
    return out


def bash() -> str:
    # On Windows a bare `bash` finds WSL's in System32 first, which cannot run this tree.
    found = shutil.which('bash') or ''
    if sys.platform == 'win32' and 'system32' in found.lower():
        git = pathlib.Path(os.environ.get('ProgramFiles', r'C:\Program Files')) / 'Git' / 'bin' / 'bash.exe'
        found = str(git) if git.exists() else found
    return found or sys.exit('no bash on the PATH')


def run_check(folder: pathlib.Path, commit: str, env_extra=None):
    env = {k: v for k, v in os.environ.items() if k not in ('SERVER_CEILING', 'INTEROP_CEILING')}
    # The check's python3 one-liners print a nuspec's BOM; a Windows console codepage cannot.
    env['PYTHONIOENCODING'] = 'utf-8'
    env.update(env_extra or {})
    cmd = [bash(), CHECK.as_posix(), folder.as_posix()] + ([commit] if commit else [])
    r = subprocess.run(cmd, capture_output=True, env=env)
    # The scratch folder's random name must not be able to satisfy a needle.
    return r.returncode, (r.stdout + r.stderr).decode('utf-8', 'replace').replace(folder.as_posix(), '<dir>')


def error_lines(out: str):
    return [l for l in out.splitlines() if l.startswith('::error::')]


def refusal(out: str, code: int, needles, any_of=()):
    """None if the check refused as wanted, else what it did instead."""
    if code == 0:
        return 'the check accepted it (exit 0)'
    errs = error_lines(out)
    for line in errs:
        low = line.lower()
        if all(n.lower() in low for n in needles) and (not any_of or any(a.lower() in low for a in any_of)):
            return None
    want = ' and '.join(f'"{n}"' for n in needles) + (' and one of ' + ' / '.join(f'"{a}"' for a in any_of) if any_of else '')
    return (f'exit {code} but no single ::error:: line has {want}; error lines: ' +
            ('; '.join(errs) if errs else 'none') + '\n' + out)


def main() -> int:
    if len(sys.argv) not in (2, 3):
        sys.exit('usage: test_check_dotnet_packages.py <folder> [<commit>]')
    source = pathlib.Path(sys.argv[1]).resolve()
    commit = sys.argv[2] if len(sys.argv) == 3 else ''
    for pid in (BASE, INTEROP, SERVER, GATEWAY, 'Eiva.Fletcher.Model'):
        package(source, pid)
    failures = 0

    def report(name: str, problem) -> None:
        nonlocal failures
        failures += problem is not None
        print(f"{'ok  ' if problem is None else 'FAIL'} {name}")
        if problem:
            print('     ' + problem.replace('\n', '\n     '))

    def scratch(tmp: str) -> pathlib.Path:
        # A fresh copy of the folder for every control, so no mutation leaks into the next.
        folder = pathlib.Path(tempfile.mkdtemp(dir=tmp))
        for f in source.iterdir():
            if f.is_file():
                shutil.copy2(f, folder / f.name)
        return folder

    with tempfile.TemporaryDirectory() as tmp:
        # The baseline: the packed folder as it is.
        folder = scratch(tmp)
        code, out = run_check(folder, commit)
        problem = None if code == 0 and not error_lines(out) else f'the unmutated folder is refused (exit {code}):\n{out}'
        report('baseline: the unmutated folder passes', problem)
        if problem:
            print('stopping: every control below would be measured against a folder that fails already')
            return 1

        # C1, the guard: native code anywhere but the two native packages.
        folder = scratch(tmp)
        injected = 'runtimes/linux-x64/native/libfletcher-c-abi.so'
        mutate(package(folder, BASE), add={injected: b'not a shim'})
        code, out = run_check(folder, commit)
        report('C1 a runtimes/ entry in Eiva.Fletcher is refused',
               refusal(out, code, [package(folder, BASE).name, injected]))

        # C2: the gateway client, which is managed-only, must not bring the server shim.
        folder = scratch(tmp)
        pkg = package(folder, GATEWAY)
        name = nuspec_name(pkg)
        mutate(pkg, replace={name: add_dependency(read(pkg, name).decode('utf-8-sig'), SERVER).encode()})
        code, out = run_check(folder, commit)
        report('C2 GatewayClient depending on the server package is refused',
               refusal(out, code, [pkg.name, SERVER], CITES))

        # C3: the server package is its two shims and nothing else.
        folder = scratch(tmp)
        extra = 'runtimes/linux-x64/native/libfletcher-extra.so'
        mutate(package(folder, SERVER), add={extra: b'a third native file'})
        code, out = run_check(folder, commit)
        report('C3 a third native file in the server package is refused',
               refusal(out, code, [package(folder, SERVER).name, extra], CITES))

        # C4: the server package needs the base package, or an app gets a shim it cannot load.
        folder = scratch(tmp)
        pkg = package(folder, SERVER)
        name = nuspec_name(pkg)
        mutate(pkg, replace={name: drop_dependency(read(pkg, name).decode('utf-8-sig'), INTEROP).encode()})
        code, out = run_check(folder, commit)
        report('C4 a server package without its Interop dependency is refused',
               refusal(out, code, [pkg.name, INTEROP], CITES))

        # C5: the server package carries its own shim's notice, which lists Arrow.
        folder = scratch(tmp)
        edge_notice = read(package(folder, INTEROP), 'THIRD-PARTY-LICENSES.txt')
        mutate(package(folder, SERVER), replace={'THIRD-PARTY-LICENSES.txt': edge_notice})
        code, out = run_check(folder, commit)
        report('C5 the edge notice in the server package is refused',
               refusal(out, code, [package(folder, SERVER).name, 'arrow']))

        # C6: the base package must not bring the server shim to an app that did not ask.
        folder = scratch(tmp)
        pkg = package(folder, BASE)
        name = nuspec_name(pkg)
        mutate(pkg, replace={name: add_dependency(read(pkg, name).decode('utf-8-sig'), SERVER).encode()})
        code, out = run_check(folder, commit)
        report('C6 Eiva.Fletcher depending on the server package is refused',
               refusal(out, code, [pkg.name, SERVER], CITES))

        # C7: the server package's size ceiling, overridable to rehearse it.
        folder = scratch(tmp)
        code, out = run_check(folder, commit, {'SERVER_CEILING': '1'})
        report('C7 a server package above its ceiling is refused',
               refusal(out, code, [package(folder, SERVER).name, 'ceiling']))

        # C8: only the server package's notice lists Arrow (the edge notice is the other
        # packages', d9 L8): an `arrow` bullet elsewhere says the wrong shim's notice was packed.
        folder = scratch(tmp)
        pkg = package(folder, INTEROP)
        server_notice = read(package(folder, SERVER), 'THIRD-PARTY-LICENSES.txt')
        mutate(pkg, replace={'THIRD-PARTY-LICENSES.txt': server_notice})
        code, out = run_check(folder, commit)
        report('C8 the server notice in a package that is not the server package is refused',
               refusal(out, code, [pkg.name, 'arrow']))

        # C9: the dependency sets are compared per target-framework group, not as one union:
        # the net10.0 group still has the Interop dependency, the net8.0 group has lost it.
        folder = scratch(tmp)
        pkg = package(folder, BASE)
        name = nuspec_name(pkg)
        mutate(pkg, replace={name: drop_dependency(read(pkg, name).decode('utf-8-sig'), INTEROP, 'net8.0').encode()})
        code, out = run_check(folder, commit)
        report('C9 Eiva.Fletcher without its Interop dependency in one group is refused',
               refusal(out, code, [pkg.name, INTEROP, 'net8.0'], CITES))

        # C10: the server package's entry allow-list. C3's extra file sits under runtimes/, which
        # the allow-list admits (the exact-set row refuses it); an assembly under lib/ is only
        # the allow-list's to refuse.
        folder = scratch(tmp)
        stray = 'lib/net8.0/Fletcher.Server.dll'
        mutate(package(folder, SERVER), add={stray: b'an assembly'})
        code, out = run_check(folder, commit)
        report('C10 an assembly in the server package is refused by its allow-list',
               refusal(out, code, [package(folder, SERVER).name, 'allow-list', stray], CITES))

        # C11: the server shim travels in the server package and nowhere else, on every path
        # (not only under runtimes/): a copy in contentFiles/ would land in an app's output
        # folder, where the loader prefers it (d10 L5).
        folder = scratch(tmp)
        stray = 'contentFiles/any/any/libfletcher-c-abi-server.so'
        mutate(package(folder, 'Eiva.Fletcher.Model'), add={stray: b'not a shim'})
        code, out = run_check(folder, commit)
        report('C11 a shim outside its own runtimes/ entries is refused',
               refusal(out, code, [package(folder, 'Eiva.Fletcher.Model').name, stray], CITES))

        # C12: a dependency on the server package outside every targetFramework group.
        folder = scratch(tmp)
        pkg = package(folder, GATEWAY)
        name = nuspec_name(pkg)
        spec = read(pkg, name).decode('utf-8-sig')
        flat = f'<dependency id="{SERVER}" version="0.5.0-alpha" />'
        mutate(pkg, replace={name: spec.replace('</dependencies>', flat + '</dependencies>').encode()})
        code, out = run_check(folder, commit)
        report('C12 a dependency on the server package outside any group is refused',
               refusal(out, code, [pkg.name, SERVER, 'outside'], CITES))

        # C13: a nuspec with no targetFramework group is refused, not read as "no dependencies".
        folder = scratch(tmp)
        pkg = package(folder, GATEWAY)
        name = nuspec_name(pkg)
        spec = read(pkg, name).decode('utf-8-sig')
        spec = spec[:spec.index('<dependencies>')] + f'<dependencies>{flat}</dependencies>' + spec[spec.index('</dependencies>') + len('</dependencies>'):]
        mutate(pkg, replace={name: spec.encode()})
        code, out = run_check(folder, commit)
        report('C13 a nuspec with no targetFramework group is refused',
               refusal(out, code, [pkg.name, 'no targetFramework dependency group'], CITES))

        # C14: the server notice is one for both platforms (ruling 2026-11-16): the pack job ships
        # one platform's notice and requires each platform's list to be inside it. Here the
        # shipped notice is the one in the server package; a platform whose graph lists a library
        # the shipped notice lacks must be refused, one that lists fewer must be accepted.
        notice = read(package(source, SERVER), 'THIRD-PARTY-LICENSES.txt').decode('utf-8')
        NL = chr(10)
        extra = '  * windows-only-library 1.0 -- MIT'
        bullets = [l for l in notice.splitlines() if l.startswith('  * ')]
        cases = {'shipped.txt': notice,
                 'equal.txt': notice,
                 'fewer.txt': notice.replace(bullets[0] + NL, '', 1),
                 'more.txt': notice.replace(bullets[0], extra + NL + bullets[0], 1)}
        with tempfile.TemporaryDirectory() as notices:
            for name, text in cases.items():
                (pathlib.Path(notices) / name).write_text(text, encoding='utf-8', newline='')
            def covers(*platform):
                cmd = [bash(), COVERS.as_posix(), (pathlib.Path(notices) / 'shipped.txt').as_posix()]
                cmd += [(pathlib.Path(notices) / p).as_posix() for p in platform]
                r = subprocess.run(cmd, capture_output=True)
                return r.returncode, (r.stdout + r.stderr).decode('utf-8', 'replace')
            code, out = covers('equal.txt', 'fewer.txt')
            problem = None if code == 0 else f'a platform list inside the shipped one was refused (exit {code}):{NL}{out}'
            code, out = covers('equal.txt', 'more.txt')
            problem = problem or refusal(out, code, ['windows-only-library'])
        report('C14 a shipped notice missing a library one platform lists is refused (and a subset accepted)', problem)

    print('every control refuses for its own reason' if failures == 0 else f'{failures} control(s) misbehaved')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
