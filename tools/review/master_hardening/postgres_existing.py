#!/usr/bin/env python3
"""Compile the current PostgreSQL adapter against an existing PostgreSQL 17.11 install.

Reuses a configured Release core and its pinned dependencies; does not modify
the PostgreSQL install. Run postgres_fixture.py separately with the new library.
"""
from pathlib import Path
import subprocess,shlex,json,argparse
p=argparse.ArgumentParser(description=__doc__)
p.add_argument("build",type=Path);p.add_argument("postgres",type=Path);p.add_argument("output",type=Path)
a=p.parse_args()
root=Path(__file__).resolve().parents[3];build=a.build.resolve();work=a.output.resolve();work.mkdir(parents=True,exist_ok=True)
pg=a.postgres.resolve()
commands=json.loads((build/'compile_commands.json').read_text());entry=next(e for e in commands if e['file']==str(root/'src/postgres_database.cpp'))
cmd=shlex.split(entry['command']);cmd[cmd.index('-o')+1]=str(work/'postgres_database.cpp.o');cmd.extend(['-DFSNET_WITH_POSTGRES','-I'+str(pg/'include')]);subprocess.run(cmd,cwd=build,check=True)
cache=(build/'CMakeCache.txt').read_text().splitlines();ninja=next(l.split('=',1)[1] for l in cache if l.startswith('CMAKE_MAKE_PROGRAM:'))
link=subprocess.check_output([ninja,'-t','commands','libfsnet.so'],cwd=build,text=True).splitlines()[-1];cmd=shlex.split(link)[2:-2]
cmd=[str(work/'postgres_database.cpp.o') if v=='CMakeFiles/fsnet.dir/src/postgres_database.cpp.o' else v for v in cmd if not v.startswith('-Wl,--dependency-file=')]
cmd[cmd.index('-o')+1]=str(work/'libfsnet.so')
i=next(i for i,v in enumerate(cmd) if v=='openssl-install/lib/libssl.a');cmd[i:i]=[str(pg/'lib/libpq.a'),str(pg/'lib/libpgcommon_shlib.a'),str(pg/'lib/libpgport_shlib.a')];cmd.append('-lm');subprocess.run(cmd,cwd=build,check=True)
subprocess.run(['c++','-std=c++20','-O2','-Iinclude','tests/postgres.cpp','-L'+str(work),'-Wl,-rpath,'+str(work),'-lfsnet','-pthread','-o',str(work/'fsnet-postgres-tests')],cwd=root,check=True)
