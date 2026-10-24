"""Install, relocate and consume exported SDK targets without source include paths."""
from pathlib import Path
import subprocess
import sys
import tempfile
cmake,build,source=sys.argv[1:]
with tempfile.TemporaryDirectory(prefix='fsnet-sdk-package-') as directory:
    root=Path(directory)
    def run(*args):subprocess.run(args,check=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=60)
    try:
        run(cmake,'--install',build,'--prefix',str(root/'original'))
        (root/'original').rename(root/'relocated')
        run(cmake,'-S',source,'-B',str(root/'consumer'),'-DCMAKE_PREFIX_PATH='+str(root/'relocated'))
        run(cmake,'--build',str(root/'consumer'),'-j2')
        run(str(root/'consumer'/'consumer'))
    except subprocess.CalledProcessError as error:
        print(error.stdout);raise
print('PASS installed SDK: relocatable fsnet::client and optional fsnet::lua targets')
