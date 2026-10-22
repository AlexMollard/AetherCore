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
# Parameter sweeps compare a non-default value against the same enabled effect at
# its native default, so the oracle isolates the parameter rather than the effect.
SWEEPS = {
    'vignette-roundness': ('BrowserPostPattern', {'vignetteIntensity': .9, 'vignetteRoundness': 0.0}, {'vignetteIntensity': .9}),
    'bloom-radius': ('BrowserPostPattern', {'bloomStrength': 1.0, 'bloomRadius': 3.0}, {'bloomStrength': 1.0}),
    'gtao-radius': ('BrowserPostCorner', {'gtao': True, 'gtaoRadius': 1.5}, {'gtao': True}),
    'gtao-strength': ('BrowserPostCorner', {'gtao': True, 'gtaoStrength': 2.0}, {'gtao': True}),
    'reflection-intensity': ('BrowserPostCorner', {'reflections': True, 'reflectionIntensity': .3}, {'reflections': True}),
    'reflection-roughness': ('BrowserPostCorner', {'reflections': True, 'reflectionMaxRoughness': .06}, {'reflections': True}),
}
FEATURES.update({name:(scene,on) for name,(scene,on,_) in SWEEPS.items()})
CONTROLS = {name:off for name,(_,_,off) in SWEEPS.items()}
# Cross-products enable several effects at once against the neutral control; the
# whole-image native/Web limits are the parity gate, the oracle proves they ran.
FEATURES.update({
    'grade-lens-stack': ('BrowserPostPattern', {'gradeContrast': 1.3, 'gradeSaturation': .6, 'vignetteIntensity': .6,
                                                 'chromaticAberration': 4.0, 'filmGrain': .5, 'bloomStrength': .8, 'fxaa': True, 'sharpness': .5}),
    'scene-stack': ('BrowserPostCorner', {'gtao': True, 'reflections': True, 'volumetrics': True, 'bloomStrength': .8}),
    'auto-exposure-key': ('BrowserPostExposure', {'autoExposure': True, 'exposureKey': .36}),
    'auto-exposure-speed': ('BrowserPostExposure', {'autoExposure': True, 'exposureSpeed': 6.0}),
})
# Roundness only differs from the aspect-following falloff on a non-square frame.
EXTENTS = {'vignette-roundness': '640x384'}
TEMPORAL = ('auto-exposure', 'auto-exposure-key', 'auto-exposure-speed', 'motion-blur')
SPATIAL = tuple(name for name in FEATURES if name not in TEMPORAL)
# Transitions start from `start`, apply `changes` (lighting-parity --changes-json; a change at tick N
# lands before update N+1) and are judged against the steady-state capture of `reference`, the same
# settings held from tick 0 at the final extent. `expect` names the transition contract:
# steady - stateless or history-carrying effects equal the steady state after enable/resize;
# adapts - exposure history restarts from manual 1 when metering is enabled late;
# manual - disabling metering returns to manual exposure 1 and the neutral image.
TRANSITIONS = {
    'gtao-enable': ('BrowserPostCorner', {}, [dict(tick=9, graphics={'gtao': True})], None, 'gtao', 'steady'),
    'ssr-enable': ('BrowserPostCorner', {}, [dict(tick=9, graphics={'reflections': True})], None, 'ssr', 'steady'),
    'bloom-resize': ('BrowserPostPattern', {'bloomStrength': 1.0}, [dict(tick=5, resize=[512, 512])], '384x320', 'bloom', 'steady'),
    'motion-blur-enable': ('BrowserPostMotion', {'motionBlurMaxRadius': 32.0}, [dict(tick=8, graphics={'motionBlur': 1.0})], None, 'motion-blur', 'steady'),
    'motion-blur-resize': ('BrowserPostMotion', {'motionBlur': 1.0, 'motionBlurMaxRadius': 32.0}, [dict(tick=5, resize=[512, 512])], '384x320', 'motion-blur', 'steady'),
    'auto-exposure-enable': ('BrowserPostExposure', {}, [dict(tick=4, graphics={'autoExposure': True})], None, 'auto-exposure', 'adapts'),
    'auto-exposure-disable': ('BrowserPostExposure', {'autoExposure': True}, [dict(tick=6, graphics={'autoExposure': False})], None, None, 'manual'),
    'auto-exposure-resize': ('BrowserPostExposure', {'autoExposure': True}, [dict(tick=5, resize=[512, 512])], '384x320', 'auto-exposure', 'adapts'),
}


def transition_checks(expect, image, reference, control, evidence, reference_evidence):
    """Judge one backend's transition capture against its steady-state reference and neutral control."""
    def mae(a, b):
        return sum(abs(x-y) for x, y in zip(a, b))/max(1, len(a))
    exposure, steady_exposure = evidence.get('exposureUsed'), (reference_evidence or {}).get('exposureUsed')
    checks = {}
    if expect == 'steady':
        checks['matchesSteadyState'] = mae(image, reference) < 1.0
        checks['effectVisible'] = mae(image, control) > mae(image, reference)
    elif expect == 'adapts':
        checks['meteringRan'] = type(exposure) in (int, float) and abs(exposure-1) > 1e-3
        # Late enable restarts from manual 1, so it adapts no further than the steady run.
        checks['noFurtherThanSteadyState'] = type(steady_exposure) in (int, float) and abs(math.log2(exposure)) <= abs(math.log2(steady_exposure))+1e-4
    elif expect == 'manual':
        checks['manualExposureRestored'] = type(exposure) in (int, float) and abs(exposure-1) < 1e-6
        checks['matchesNeutralControl'] = mae(image, control) < 1.0
    else:
        raise ValueError('Unknown transition contract: '+expect)
    return dict(passed=all(checks.values()), checks=checks, exposureUsed=exposure, steadyExposureUsed=steady_exposure,
                steadyMae=mae(image, reference) if reference is not None else None, controlMae=mae(image, control))


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
    elif feature == 'vignette-roundness':
        # Wide frame: roundness 1 stretches x by the aspect (darker left/right edges);
        # 0 follows the frame. The vertical axis is untouched either way. The authored
        # content spans only the middle of a wide frame, so probe its edges, not black.
        side=min(sum(p(on,x,32))-sum(p(off,x,32)) for x in (16,48))
        top=sum(p(on,32,2))-sum(p(off,32,2))
        checks['aspectFollowingFalloffBrightensSides'] = side>5
        checks['verticalFalloffUnchanged'] = abs(top)<4
        metrics.update(sideGain=side,topChange=top)
    elif feature == 'bloom-radius':
        # A wider upsample filter carries the highlight farther from the marker.
        far=sum(p(on,40,16))-sum(p(off,40,16))
        checks['widerFilterSpreadsFarther'] = far>2
        metrics['farSpreadGain']=far
    elif feature == 'gtao-strength':
        darkened=sum(sum(p(off,x,y))-sum(p(on,x,y))>6 for y in range(20,57) for x in range(8,57))
        checks['largerOcclusionDarkensMore'] = darkened>=4
        metrics['darkenedProbePixels']=darkened
    elif feature == 'gtao-radius':
        # A wider horizon search only reaches the thin contact bands the default radius
        # misses, so the probe grid undersamples it: count every pixel instead.
        delta=[sum(off[i:i+3])-sum(on[i:i+3]) for i in range(0,width*height*4,4)]
        darkened,lightened=sum(d>6 for d in delta),sum(d<-6 for d in delta)
        checks['widerSearchOnlyDarkens'] = darkened>=32 and lightened==0
        metrics.update(darkenedPixels=darkened,lightenedPixels=lightened)
    elif feature in ('reflection-intensity','reflection-roughness'):
        lost=sum(sum(p(off,x,y))-sum(p(on,x,y))>6 for y in range(35,60) for x in range(6,59))
        checks['weakerReflectionRemovesFloorRadiance'] = lost>=4
        metrics['reflectionLossProbePixels']=lost
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


def temporal_checks(feature,on,off,width,height,on_case,off_case,backend,static=None,graphics=None):
    evidence_key='nativeFrameEvidence' if backend=='native' else 'webFrameEvidence'
    enabled,control=on_case.get(evidence_key),off_case.get(evidence_key)
    if not isinstance(enabled,dict) or not isinstance(control,dict):
        return dict(passed=False,checks=dict(acceptedFrameMetadataAvailable=False),diagnostic='Actual screenshot frame metadata is required')
    other=on_case.get('webFrameEvidence' if backend=='native' else 'nativeFrameEvidence') or {}
    frame=enabled.get('frameIndex') if backend=='native' else enabled.get('temporalFrameIndex')
    other_frame=other.get('temporalFrameIndex') if backend=='native' else other.get('frameIndex')
    checks=dict(acceptedFramesMatch=type(frame) is int and frame==other_frame,
                capturedFinalTick=enabled.get('simulationTick')==10 if backend=='web' else abs(enabled.get('elapsedTime',-1)-10/60)<1e-4)
    if feature.startswith('auto-exposure'):
        key,speed=(graphics or {}).get('exposureKey',.18),(graphics or {}).get('exposureSpeed',2.0)
        expected=expected_exposure(frame,key=key,speed=speed) if type(frame) is int and 0<=frame<=600 else None
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


def run_capture(scene, overrides, label, args, directory, deadline, worker_factory, extent=None, changes=None, initial_extent=None):
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
    command+=['--extent',extent or getattr(args,'extent','512x512')]
    if changes:
        schedule=directory/(label+'-changes.json')
        schedule.write_text(json.dumps(changes,indent=2)+'\n',encoding='utf-8')
        command+=['--changes-json',str(schedule)]
    if initial_extent:
        command+=['--initial-extent',initial_extent]
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
    parser.add_argument('--features',default=','.join([*FEATURES,*TRANSITIONS]),help='Explicit maintained spatial/temporal/transition feature names')
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
        if not features or len(set(features))!=len(features) or any(name not in FEATURES and name not in TRANSITIONS for name in features):
            raise ValueError('--features must contain unique maintained feature names')
        if not 30<=args.timeout<=3600:
            raise ValueError('--timeout must be30..3600')
        directory=args.report.parent/('post-parity-'+uuid.uuid4().hex)
        directory.mkdir()
        report.update(artifacts=str(directory),requestedFeatures=features)
        sys.path.insert(0,str(ROOT/'tools/project'))
        from gameplay_jobs import _OwnedProcess
        controls={}
        def cached(scene,overrides,label,extent):
            key=(scene,json.dumps(overrides,sort_keys=True),extent)
            if key not in controls:
                controls[key]=run_capture(scene,overrides,label,args,directory,started+args.timeout,_OwnedProcess,extent)
            return controls[key]
        def backend_image(result,scene,backend):
            artifacts=Path(result['report']['artifacts'])
            width,height,image=parity.png(artifacts/(scene+'-native.png'))
            return width,height,(artifacts/(scene+'-web.rgba')).read_bytes() if backend=='web' else image
        for feature in features:
            if feature in TRANSITIONS:
                scene,start,changes,initial,reference_feature,expect=TRANSITIONS[feature]
                item=dict(feature=feature,scene=scene,status='failed',extent=args.extent,initialExtent=initial,changes=changes,
                          startGraphics=parity.graphics_settings(start),expect=expect,reference=reference_feature)
                report['cases'].append(item)
                control=cached(scene,{},scene+'-off',args.extent)
                reference=cached(scene,FEATURES[reference_feature][1],reference_feature+'-steady',args.extent) if reference_feature else None
                transition=run_capture(scene,start,feature+'-transition',args,directory,started+args.timeout,_OwnedProcess,args.extent,changes,initial)
                runs=[control,transition]+([reference] if reference else [])
                item.update(controlReport=control['reportPath'],transitionReport=transition['reportPath'],referenceReport=reference['reportPath'] if reference else None)
                if any(run['returnCode']!=0 or run['report']['status']!='passed' for run in runs):
                    item['diagnostic']='Maintained backend image/probe qualification failed; inspect child reports'
                    continue
                outputs={}
                for backend in ('native','web'):
                    key='nativeFrameEvidence' if backend=='native' else 'webFrameEvidence'
                    images=[backend_image(run,scene,backend) for run in runs]
                    if len({image[:2] for image in images})!=1:
                        raise ValueError('Transition/control/reference output extents differ: '+feature)
                    outputs[backend]=transition_checks(expect,images[1][2],images[2][2] if reference else None,images[0][2],
                        transition['report']['cases'][0].get(key) or {},reference['report']['cases'][0].get(key) if reference else None)
                item.update(backends=outputs,status='passed' if all(value['passed'] for value in outputs.values()) else 'failed')
                continue
            scene,overrides=FEATURES[feature]
            off_overrides=CONTROLS.get(feature,{})
            extent=EXTENTS.get(feature,args.extent)
            item=dict(feature=feature,scene=scene,status='failed',extent=extent,
                      onGraphics=parity.graphics_settings(overrides),offGraphics=parity.graphics_settings(off_overrides))
            report['cases'].append(item)
            control_key=(scene,json.dumps(off_overrides,sort_keys=True),extent)
            if control_key not in controls:
                label=scene+'-off' if not off_overrides and extent==args.extent else feature+'-control'
                controls[control_key]=run_capture(scene,off_overrides,label,args,directory,started+args.timeout,_OwnedProcess,extent)
            control=controls[control_key]
            enabled=run_capture(scene,overrides,feature+'-on',args,directory,started+args.timeout,_OwnedProcess,extent)
            item.update(controlReport=control['reportPath'],enabledReport=enabled['reportPath'])
            if control['returnCode']!=0 or enabled['returnCode']!=0 or control['report']['status']!='passed' or enabled['report']['status']!='passed':
                item['diagnostic']='Maintained backend image/probe qualification failed; inspect child reports'
                continue
            outputs={}
            static_control=None
            if feature=='motion-blur':
                static_key=('BrowserPostPattern','{}',args.extent)
                if static_key not in controls:
                    controls[static_key]=run_capture('BrowserPostPattern',{},'BrowserPostPattern-off',args,directory,started+args.timeout,_OwnedProcess)
                static_control=controls[static_key]
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
                        enabled['report']['cases'][0],control['report']['cases'][0],backend,static,item['onGraphics'])
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
