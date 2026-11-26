import plistlib,os,json,sys
R='/root/unixnu-work/apple-trees/macos-full/root/System/Library'
out={}
for d in ('LaunchDaemons','LaunchAgents'):
    for f in os.listdir(f'{R}/{d}'):
        try: p=plistlib.load(open(f'{R}/{d}/{f}','rb'))
        except Exception as e: continue
        lab=p.get('Label',f[:-6])
        prog=p.get('Program') or (p.get('ProgramArguments') or [''])[0]
        out[lab]=dict(dom=d[6:],file=f,prog=os.path.basename(prog),da=p.get('DisableAllowed'),ral=p.get('RunAtLoad'),ka=bool(p.get('KeepAlive')),lsc=p.get('LimitLoadToSessionType'),dis=p.get('Disabled'))
json.dump(out,open(sys.argv[1],'w'),indent=0)
print(len(out))
