"""Independent display-response probes for all eleven public tonemap choices.

The authored fixture contains constant neutral radiance bands. Scalar curves are
evaluated on CPU, including the documented display-gamma cancellation of Hejl,
Dice and AgX. No engine shader compiler or renderer code is loaded by this oracle.
"""
import math

SCENE='BrowserPostTonemap'
RADIANCES=(0.0,.004,.01,.05,.2,.5,2.0,8.0)
MODES=('Reinhard','ACES Filmic','Uncharted 2','Hejl Richard','Linear','Exponential',
       'Filmic Dice','Lottes','RomBinDaHouse','Vanilla','AgX')
PROFILES={
    'tonemap-reinhard':('Reinhard'), 'tonemap-aces':('ACES Filmic'),
    'tonemap-uncharted':('Uncharted 2'), 'tonemap-hejl':('Hejl Richard'),
    'tonemap-linear':('Linear'), 'tonemap-exponential':('Exponential'),
    'tonemap-dice':('Filmic Dice'), 'tonemap-lottes':('Lottes'),
    'tonemap-rombin':('RomBinDaHouse'), 'tonemap-vanilla':('Vanilla'), 'tonemap-agx':('AgX'),
}


def clamp(value):
    return max(0,min(1,value))


def display_rgb(mode,radiance):
    x=max(0,radiance)
    encoded=False
    if mode=='Reinhard': value=x/(x+1)
    elif mode=='ACES Filmic': value=(x*(x+.0245786)-.000090537)/(x*(.983729*x+.432951)+.238081)
    elif mode=='Uncharted 2':
        partial=lambda v:(v*(.15*v+.05)+.004)/(v*(.15*v+.5)+.06)-.02/.3
        value=partial(x*2)/partial(11.2)
    elif mode in ('Hejl Richard','Filmic Dice'):
        v=max(0,x-.004) if mode=='Hejl Richard' else x
        value=v*(6.2*v+.5)/(v*(6.2*v+1.7)+.06)
        encoded=True
    elif mode in ('Linear','Vanilla'): value=x
    elif mode=='Exponential': value=1-math.exp(-x)
    elif mode=='Lottes': value=x*(1.47*x+.12)/(x*(1.47*x+1.46)+.12)
    elif mode=='RomBinDaHouse': value=x*(.26*x+.68)/(x*(.26*x+1.14)+.14)
    elif mode=='AgX':
        inset=((.842479062253094,.0784336,.0792237451477643),
               (.0423282422610123,.878468636469772,.0791661274605434),
               (.0423756549057051,.0784336,.879142973793104))
        outset=((1.19687900512017,-.0980208811401368,-.0990297440797205),
                (-.0528968517574562,1.15190312990417,-.0989611768448433),
                (-.0529716355144438,-.0980434501171241,1.15107367264116))
        contrast=lambda v:clamp(15.5*v**6-40.14*v**5+31.96*v**4-6.868*v**3+.4298*v**2+.1191*v-.00232)
        working=[contrast((max(-12.47393,min(4.026069,math.log2(max(x*sum(row),1e-10))))+12.47393)/16.499999) for row in inset]
        return [255*clamp(sum(a*b for a,b in zip(row,working))) for row in outset]
    else: raise ValueError('Unknown tonemap mode: '+mode)
    display=clamp(value) if encoded else clamp(value)**(1/2.2)
    return [255*display]*3


def checks(feature,on,off,width,height,patch):
    mode=PROFILES[feature]
    probes=[]
    for index,radiance in enumerate(RADIANCES):
        expected=display_rgb(mode,radiance)
        control_expected=display_rgb('ACES Filmic',radiance)
        actual=patch(on,width,height,4+index*8,32,False)
        control=patch(off,width,height,4+index*8,32,False)
        probes.append(dict(radiance=radiance,actual=actual,expected=expected,
                           control=control,controlExpected=control_expected,
                           maximumError=max(abs(a-b) for a,b in zip(actual,expected)),
                           controlMaximumError=max(abs(a-b) for a,b in zip(control,control_expected))))
    result=dict(allEightRadianceBandsMatchAnalyticResponse=all(p['maximumError']<=3 for p in probes),
                neutralControlMatchesAces=all(p['controlMaximumError']<=3 for p in probes),
                finiteBoundedDisplay=all(math.isfinite(c) and 0<=c<=255 for p in probes for c in p['actual']),
                retainsHdrResponse=max(probes[-1]['actual'])>max(probes[4]['actual'])+20)
    if mode!='ACES Filmic':
        result['selectedCurveDoesNotFallBackToAces']=max(max(abs(a-b) for a,b in zip(p['actual'],p['control'])) for p in probes)>4
    return dict(passed=all(result.values()),checks=result,mode=mode,probes=probes,displayChannelTolerance=3,
                intentionalDisplayAlias='Linear/Vanilla' if mode in ('Linear','Vanilla') else None)
