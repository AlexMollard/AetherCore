"""Qualify explicit post-effect settings against neutral controls on both backends.

Uses owned private-project runs from lighting-parity-checks.py, preserving its
unchanged whole-image and probe tolerances. Does not publish or alter user settings.
Temporal effects require accepted-frame/resource capture and analytic metering evidence.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import math
from pathlib import Path
import statistics
import sys
import time
import uuid
import tonemap_post_oracles

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('lighting_parity', Path(__file__).with_name('lighting-parity-checks.py'))
parity = importlib.util.module_from_spec(spec)
spec.loader.exec_module(parity)

# Each on map changes exactly one native control over the neutral map, apart
# from motion blur's explicit radius. Scene authoring is shared, settings are not
# baked into a second production project or silently inferred from scene names.
FEATURES = {
    'contrast': ('BrowserPostPattern', {'gradeContrast': 1.6}),
    'saturation': ('BrowserPostPattern', {'gradeSaturation': 0.0}),
    'temperature': ('BrowserPostPattern', {'gradeTemperature': .7}),
    'tint': ('BrowserPostPattern', {'gradeTint': .7}),
    'vignette': ('BrowserPostPattern', {'vignetteIntensity': .9}),
    'chromatic': ('BrowserPostPattern', {'chromaticAberration': 8.0}),
    'grain': ('BrowserPostPattern', {'filmGrain': 1.0}),
    'bloom': ('BrowserPostPattern', {'bloomStrength': 1.0}),
    'fxaa': ('BrowserPostPattern', {'fxaa': True}),
    'sharpen': ('BrowserPostPattern', {'sharpness': 1.0}),
    'render-scale': ('BrowserPostPattern', {'renderScale': .5}),
    'gtao': ('BrowserPostCorner', {'gtao': True}),
    'ssr': ('BrowserPostCorner', {'reflections': True}),
    'volumetric': ('BrowserPostVolumetric', {'volumetrics': True}),
    'display-fxaa': ('BrowserPostDisplay', {'fxaa': True}),
    'display-sharpen': ('BrowserPostDisplay', {'sharpness': 1.0}),
    'display-render-scale': ('BrowserPostDisplay', {'renderScale': .5}),
    'auto-exposure': ('BrowserPostExposure', {'autoExposure': True}),
    'motion-blur': ('BrowserPostMotion', {'motionBlur': 1.0, 'motionBlurMaxRadius': 32.0}),
}
FEATURES.update({feature:(tonemap_post_oracles.SCENE,{'tonemap':mode}) for feature,mode in tonemap_post_oracles.PROFILES.items()})
TEMPORAL = ('auto-exposure', 'motion-blur')
SPATIAL = tuple(name for name in FEATURES if name not in TEMPORAL)


def effect_checks(feature, on, off, width, height):
    if feature not in SPATIAL:
        raise ValueError('Temporal qualification requires accepted-frame capture: '+feature)
    if len(on) != width*height*4 or len(off) != len(on):
        raise ValueError('Post effect/control capture extents differ')
    if feature in tonemap_post_oracles.PROFILES:
        return tonemap_post_oracles.checks(feature,on,off,width,height,parity.patch)
    p = lambda image,x,y: parity.patch(image,width,height,x,y,False)
    deltas = [max(abs(a-b) for a,b in zip(p(on,x,y),p(off,x,y)))
              for y in range(2,62) for x in range(2,62)]
    changed = sum(delta > 2 for delta in deltas)
    checks = dict(effectChangesPixels=changed >= 4,
                  controlSceneVisible=max(p(off,16,32)+p(off,48,32)) > 40)
    metrics = dict(changedProbePixels=changed, differenceThresholdExclusive=2,
                   minimumChangedProbePixels=4, maximumProbeDifference=max(deltas))
    if feature == 'saturation':
        colors = [p(on,16,32),p(on,48,32)]
        checks['coloredRegionsBecomeNeutral'] = all(max(c)-min(c)<4 for c in colors)
        checks['controlHasColor'] = max(p(off,16,32))-min(p(off,16,32))>40
    elif feature == 'temperature':
        warm,neutral=p(on,32,32),p(off,32,32)
        checks['whiteBalanceWarmsGray'] = warm[0]-warm[2]>20 and abs(neutral[0]-neutral[2])<4
    elif feature == 'tint':
        tinted=p(on,32,32)
        checks['whiteBalanceRaisesGreen'] = tinted[1]-max(tinted[0],tinted[2])>10
    elif feature == 'contrast':
        checks['abovePivotRadianceBrightens'] = p(on,16,32)[0]-p(off,16,32)[0]>5
    elif feature == 'vignette':
        edge=sum(p(off,4,4))-sum(p(on,4,4))
        center=sum(p(off,32,32))-sum(p(on,32,32))
        checks['lensDarkensCornersMoreThanCenter'] = edge>30 and edge>center+20
        metrics.update(cornerDarkening=edge,centerDarkening=center)
    elif feature == 'bloom':
        # Marker world x[-.25,.25],y[1.75,2.25]; point(36,16) is
        # outside that geometry on the gray receiver, so only spread can lift it.
        spread=sum(p(on,36,16))-sum(p(off,36,16))
        checks['hdrHighlightSpreadsOutsideGeometry'] = spread>5
        metrics['outsideHighlightGain']=spread
    elif feature == 'chromatic':
        # The lower diagonal and thin white stripes are achromatic sources.
        # Channel separation must create fringes there, not merely recolor a patch.
        fringe=sum(max(p(on,x,y))-min(p(on,x,y))>max(p(off,x,y))-min(p(off,x,y))+3
                   for y in range(38,57) for x in range(8,57))
        checks['whiteEdgesAcquireChromaticFringes'] = fringe>=4
        metrics['fringeProbePixels']=fringe
    elif feature == 'grain':
        residual=[on[(y*width+x)*4]-off[(y*width+x)*4]
                  for y in range(height//8,7*height//8) for x in range(width//32,width//8)]
        checks['midtoneNoiseHasBothSigns'] = min(residual)<-2 and max(residual)>2
        checks['noiseDoesNotApplyBrightnessOffset'] = abs(statistics.mean(residual))<2
        checks['visibleNoiseVariance'] = statistics.pstdev(residual)>.5
        metrics.update(noiseMean=statistics.mean(residual),noiseStandardDeviation=statistics.pstdev(residual))
    elif feature in ('fxaa','sharpen','render-scale','display-fxaa','display-sharpen','display-render-scale'):
        if feature.startswith('display-'):
            region_y,region_x=range(20,43),range(20,44)
        else:
            region_y,region_x=range(37,59),range(6,59)
        edge_changes=sum(max(abs(a-b) for a,b in zip(p(on,x,y),p(off,x,y)))>2
                         for y in region_y for x in region_x)
        checks['highFrequencyGeometryChanges'] = edge_changes>=4
        checks['flatRadiancePreserved'] = max(abs(a-b) for a,b in zip(p(on,4,4),p(off,4,4)))<4
        metrics['edgeChangedProbePixels']=edge_changes
    elif feature == 'gtao':
        darkened=sum(sum(p(off,x,y))-sum(p(on,x,y))>6 for y in range(20,57) for x in range(8,57))
        checks['depthCornerOcclusionDarkensSurfaces'] = darkened>=4
        metrics['darkenedProbePixels']=darkened
    elif feature == 'ssr':
        gained=sum(sum(p(on,x,y))-sum(p(off,x,y))>6 for y in range(35,60) for x in range(6,59))
        checks['reflectionAddsRadianceOnFloor'] = gained>=4
        metrics['reflectionProbePixels']=gained
    elif feature == 'volumetric':
        gained=sum(sum(p(on,x,y))-sum(p(off,x,y))>3 for y in range(8,56) for x in range(8,56))
        checks['participatingAirAddsScatteredSunlight'] = gained>=4
        metrics['scatteringProbePixels']=gained
    return dict(passed=all(checks.values()),checks=checks,**metrics)


def expected_exposure(frame_index, radiance=2.0, key=.18, speed=2.0):
    bin_index=int(max(0,min(255,(math.log2(radiance)+10)*255/20)))
    target=max(.03,min(30,key/2**(-10+bin_index/255*20)))
    value=1.0
    # Native records0,6,12... and reads that same3-frame slot at3,9,15...
    for frame in range(frame_index+1):
        if frame>=3 and (frame-3)%6==0:
            value=2**(math.log2(value)+(math.log2(target)-math.log2(value))*(1-math.exp(-speed/60)))
    return value


def temporal_checks(feature,on,off,width,height,on_case,off_case,backend,static=None):
    evidence_key='nativeFrameEvidence' if backend=='native' else 'webFrameEvidence'
    enabled,control=on_case.get(evidence_key),off_case.get(evidence_key)
    if not isinstance(enabled,dict) or not isinstance(control,dict):
        return dict(passed=False,checks=dict(acceptedFrameMetadataAvailable=False),diagnostic='Actual screenshot frame metadata is required')
    other=on_case.get('webFrameEvidence' if backend=='native' else 'nativeFrameEvidence') or {}
    frame=enabled.get('frameIndex') if backend=='native' else enabled.get('temporalFrameIndex')
    other_frame=other.get('temporalFrameIndex') if backend=='native' else other.get('frameIndex')
    checks=dict(acceptedFramesMatch=type(frame) is int and frame==other_frame,
                capturedFinalTick=enabled.get('simulationTick')==10 if backend=='web' else abs(enabled.get('elapsedTime',-1)-10/60)<1e-4)
    if feature=='auto-exposure':
        expected=expected_exposure(frame) if type(frame) is int and 0<=frame<=600 else None
        actual=enabled.get('exposureUsed')
        checks['gpuExposureEvidenceFinite']=type(actual) in (int,float) and math.isfinite(actual)
        checks['matchesAnalyticHistogramAndThreeSlotCadence']=expected is not None and checks['gpuExposureEvidenceFinite'] and abs(actual-expected)<2e-4
        checks['neutralControlKeepsManualExposure']=abs(control.get('exposureUsed',-1)-1)<1e-6
        checks['adaptationChangesDisplay']=sum(abs(a-b)>2 for a,b in zip(parity.patch(on,width,height,32,32),parity.patch(off,width,height,32,32)))>=2
        return dict(passed=all(checks.values()),checks=checks,evidence=enabled,expectedExposure=expected,histogramBin=140,
                    exposureAbsoluteTolerance=.0002,meterRadiance=2,grid=[256,144],framesInFlight=3,meterPeriod=6)
    filtered=effect_checks('fxaa',on,off,width,height)
    checks.update(filtered['checks'])
    if static is None:
        checks['movingCameraControlAvailable']=False
    else:
        pose_changes=sum(max(abs(a-b) for a,b in zip(parity.patch(off,width,height,x,y,False),parity.patch(static,width,height,x,y,False)))>5
                         for y in range(4,60) for x in range(4,60))
        checks['authoredRustCameraActuallyMoves']=pose_changes>=8
    return dict(passed=all(checks.values()),checks=checks,evidence=enabled,
                edgeChangedProbePixels=filtered['edgeChangedProbePixels'])


def run_capture(scene, overrides, label, args, directory, deadline, worker_factory):
    graphics=parity.graphics_settings(overrides)
    settings=directory/(label+'-graphics.json')
    settings.write_text(json.dumps(graphics,indent=2)+'\n',encoding='utf-8')
    destination=directory/(label+'.json')
    remaining=int(deadline-time.monotonic())
    if remaining<30:
        raise TimeoutError('Post qualification shared deadline exhausted')
    command=[sys.executable,str(Path(__file__).with_name('lighting-parity-checks.py')),
             '--build',str(args.build),'--report',str(destination),'--scenes',scene,
             '--graphics-json',str(settings),'--timeout',str(min(remaining,1200))]
    command+=['--extent',getattr(args,'extent','512x512')]
    command+=['--isolate-render-scale']
    if args.playwright:
        command+=['--playwright',args.playwright]
    with (directory/(label+'.log')).open('wb') as stream:
        worker=worker_factory(command,ROOT,stream)
        try:
            while worker.poll() is None:
                if time.monotonic()>=deadline:
                    raise TimeoutError('Post capture exceeded shared deadline')
                time.sleep(.05)
            if not destination.is_file():
                raise RuntimeError('Post capture produced no maintained report: '+label)
            report=json.loads(destination.read_text(encoding='utf-8'))
            return dict(reportPath=str(destination),returnCode=worker.poll(),report=report)
        finally:
            worker.close()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build',type=Path,default=ROOT/'build/webgpu-browser')
    parser.add_argument('--report',type=Path,required=True)
    parser.add_argument('--features',default=','.join(FEATURES),help='Explicit maintained spatial/temporal feature names')
    parser.add_argument('--timeout',type=int,default=1200,help='Shared deadline30..3600seconds')
    parser.add_argument('--playwright')
    parser.add_argument('--extent',default='512x512',help='Exact shared output WIDTHxHEIGHT; use513x511 for odd half-resolution resources')
    args=parser.parse_args()
    report=dict(schemaVersion=1,operation='post-parity',status='failed',cases=[],diagnostics=[],
                scope='Selected authored post effects, independent on/off backend semantics and unchanged image parity limits; no publishing qualification')
    started=time.monotonic()
    args.report=args.report.resolve()
    args.report.parent.mkdir(parents=True,exist_ok=True)
    try:
        features=args.features.split(',')
        if not features or len(set(features))!=len(features) or any(name not in FEATURES for name in features):
            raise ValueError('--features must contain unique maintained feature names')
        if not 30<=args.timeout<=3600:
            raise ValueError('--timeout must be30..3600')
        directory=args.report.parent/('post-parity-'+uuid.uuid4().hex)
        directory.mkdir()
        report.update(artifacts=str(directory),requestedFeatures=features)
        sys.path.insert(0,str(ROOT/'tools/project'))
        from gameplay_jobs import _OwnedProcess
        controls={}
        for feature in features:
            scene,overrides=FEATURES[feature]
            item=dict(feature=feature,scene=scene,status='failed',onGraphics=parity.graphics_settings(overrides),offGraphics=parity.graphics_settings({}))
            report['cases'].append(item)
            if scene not in controls:
                controls[scene]=run_capture(scene,{},scene+'-off',args,directory,started+args.timeout,_OwnedProcess)
            control=controls[scene]
            enabled=run_capture(scene,overrides,feature+'-on',args,directory,started+args.timeout,_OwnedProcess)
            item.update(controlReport=control['reportPath'],enabledReport=enabled['reportPath'])
            if control['returnCode']!=0 or enabled['returnCode']!=0 or control['report']['status']!='passed' or enabled['report']['status']!='passed':
                item['diagnostic']='Maintained backend image/probe qualification failed; inspect child reports'
                continue
            outputs={}
            static_control=None
            if feature=='motion-blur':
                if 'BrowserPostPattern' not in controls:
                    controls['BrowserPostPattern']=run_capture('BrowserPostPattern',{},'BrowserPostPattern-off',args,directory,started+args.timeout,_OwnedProcess)
                static_control=controls['BrowserPostPattern']
                if static_control['report']['status']!='passed':
                    item['diagnostic']='Stationary camera comparison failed'
                    continue
            for backend in ('native','web'):
                images=[]
                for result in (enabled,control):
                    artifacts=Path(result['report']['artifacts'])
                    width,height,image=parity.png(artifacts/(scene+'-native.png'))
                    if backend=='web':
                        image=(artifacts/(scene+'-web.rgba')).read_bytes()
                    images.append((width,height,image))
                if images[0][:2]!=images[1][:2]:
                    raise ValueError('Post on/off output extents differ: '+feature)
                if feature in TEMPORAL:
                    static=None
                    if static_control:
                        artifacts=Path(static_control['report']['artifacts'])
                        sw,sh,static=parity.png(artifacts/'BrowserPostPattern-native.png')
                        if (sw,sh)!=images[0][:2]:
                            raise ValueError('Stationary/moving camera capture extents differ')
                        if backend=='web':
                            static=(artifacts/'BrowserPostPattern-web.rgba').read_bytes()
                    outputs[backend]=temporal_checks(feature,images[0][2],images[1][2],*images[0][:2],
                        enabled['report']['cases'][0],control['report']['cases'][0],backend,static)
                else:
                    outputs[backend]=effect_checks(feature,images[0][2],images[1][2],*images[0][:2])
            item.update(backends=outputs,status='passed' if all(value['passed'] for value in outputs.values()) else 'failed')
        report['status']='passed' if report['cases'] and all(item['status']=='passed' for item in report['cases']) else 'failed'
    except (Exception,KeyboardInterrupt) as error:
        report['diagnostics'].append(dict(code='POST_PARITY',message=str(error)))
    finally:
        report['durationMs']=round((time.monotonic()-started)*1000)
        args.report.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(dict(status=report['status'],report=str(args.report))))
    return 0 if report['status']=='passed' else 1


if __name__=='__main__':
    raise SystemExit(main())
