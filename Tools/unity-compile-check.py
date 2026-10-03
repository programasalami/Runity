"""Compile-checks the Unity C# assemblies with the .NET SDK, without starting Unity.

It copies Unity's generated SDK-style .csproj files into a scratch folder, swaps their explicit <Compile> lists for globs of the
assembly folder (so new files are included) and points relative paths back at the project. Then `dotnet build` the requested one.
Usage: python Tools/unity-compile-check.py WaW.Presentation   (needs the Unity-generated WaW/*.csproj files)
"""
import os
import re
import subprocess
import sys

PROJECT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'WaW')
OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'TestResults', 'unity-compile-check')
FOLDERS = {
    'WaW.Protocol': r'Assets\Scripts\Protocol',
    'WaW.Net': r'Assets\Scripts\Net',
    'WaW.Domain': r'Assets\Scripts\Domain',
    'WaW.Client': r'Assets\Scripts\Client',
    'WaW.Presentation': r'Assets\Scripts\Presentation',
    'WaW.EditorTools': r'Assets\Editor',
}

os.makedirs(OUT, exist_ok=True)
for name, folder in FOLDERS.items():
    src = os.path.join(PROJECT, name + '.csproj')
    s = open(src, encoding='utf-8-sig').read()
    s = re.sub(r'\s*<Compile Include="[^"]*" />', '', s)
    glob = os.path.join(PROJECT, folder, '**', '*.cs')
    s = s.replace('<ItemGroup>\n    <None', '<ItemGroup>\n    <Compile Include="%s" />\n    <None' % glob, 1) if '<None' in s else s
    if 'Compile Include="%s"' % glob not in s:
        s = s.replace('</Project>', '  <ItemGroup>\n    <Compile Include="%s" />\n  </ItemGroup>\n</Project>' % glob)
    s = re.sub(r'<None Include="[^"]*" />', '', s)
    s = s.replace('Temp\\obj\\', os.path.join(OUT, 'obj') + '\\').replace('Temp\\bin\\Debug\\', os.path.join(OUT, 'bin') + '\\')
    s = re.sub(r'<HintPath>(?![A-Za-z]:)([^<]+)</HintPath>', lambda m: '<HintPath>%s</HintPath>' % os.path.join(PROJECT, m.group(1)), s)
    s = re.sub(r'<ProjectReference Include="([^"]+)\.csproj"', lambda m: '<ProjectReference Include="%s.csproj"' % os.path.join(OUT, os.path.basename(m.group(1))), s)
    open(os.path.join(OUT, name + '.csproj'), 'w', encoding='utf-8').write(s)

target = sys.argv[1] if len(sys.argv) > 1 else 'WaW.Presentation'
r = subprocess.run(['dotnet', 'build', os.path.join(OUT, target + '.csproj'), '-v', 'q', '-nologo', '-clp:NoSummary'],
                   capture_output=True, text=True)
lines = [l for l in (r.stdout + r.stderr).splitlines() if ' error ' in l or 'warning CS0' in l and 'Assets' in l]
seen = set()
for l in lines:
    l = re.sub(r'\s*\[[^\]]*\.csproj\]$', '', l)
    if l not in seen:
        seen.add(l)
        print(l)
print('exit', r.returncode)
