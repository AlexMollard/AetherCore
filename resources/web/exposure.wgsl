// Native luminance_histogram.slang footprint and PostProcessStack adaptation.
// Adapt a recycled native3-frame slot BEFORE tonemap; meter every6frames AFTER.
struct Settings {
    width:u32, height:u32, meterWidth:u32, meterHeight:u32,
    key:f32, speed:f32, enabled:u32, pad:u32,
};
@group(0) @binding(0) var hdr:texture_2d<f32>;
@group(0) @binding(1) var linearSampler:sampler;
@group(0) @binding(2) var<storage,read_write> bins:array<atomic<u32>,256>;
@group(0) @binding(3) var<storage,read_write> exposure:array<f32>;
@group(0) @binding(4) var<uniform> settings:Settings;

@compute @workgroup_size(256)
fn histogram(@builtin(global_invocation_id) id:vec3<u32>) {
    let index=id.x;
    if(index>=settings.meterWidth*settings.meterHeight) { return; }
    let rcp=1.0/vec2<f32>(f32(settings.meterWidth),f32(settings.meterHeight));
    let uvMin=vec2<f32>(f32(index%settings.meterWidth),f32(index/settings.meterWidth))*rcp;
    let footprint=vec2<f32>(f32(settings.width),f32(settings.height))*rcp;
    let taps=clamp(vec2<u32>(ceil(footprint*0.5)),vec2<u32>(1),vec2<u32>(2));
    let step=rcp/vec2<f32>(taps);
    let halfTexel=0.5/vec2<f32>(f32(settings.width),f32(settings.height));
    var sum=vec3<f32>(0.0);
    for(var y=0u;y<taps.y;y++) {
        for(var x=0u;x<taps.x;x++) {
            let uv=clamp(uvMin+(vec2<f32>(f32(x),f32(y))+0.5)*step,halfTexel,1.0-halfTexel);
            sum+=textureSampleLevel(hdr,linearSampler,uv,0.0).rgb;
        }
    }
    let color=sum/f32(taps.x*taps.y);
    let logLum=log2(max(dot(color,vec3<f32>(0.2126,0.7152,0.0722)),1e-6));
    let bin=u32(clamp((logLum+10.0)*(255.0/20.0),0.0,255.0));
    atomicAdd(&bins[bin],1u);
}

@compute @workgroup_size(1)
fn adapt() {
    if(settings.enabled==0u) { return; }
    var total=0.0;
    for(var i=1u;i<256u;i++) { total+=f32(atomicLoad(&bins[i])); }
    if(total<1.0) { return; }
    let low=total*0.30;
    let high=total*0.95;
    var seen=0.0;
    var weighted=0.0;
    var weight=0.0;
    for(var i=1u;i<256u;i++) {
        let count=f32(atomicLoad(&bins[i]));
        if(count<=0.0) { continue; }
        let start=seen;
        seen+=count;
        let lo=max(start,low);
        let hi=min(seen,high);
        if(hi<=lo) { continue; }
        let logLum=-10.0+(f32(i)/255.0)*20.0;
        weighted+=logLum*(hi-lo);
        weight+=hi-lo;
    }
    if(weight<=0.0) { return; }
    let luminance=exp2(weighted/weight);
    let targetExposure=clamp(settings.key/max(luminance,1e-4),0.03,30.0);
    let blend=1.0-exp(-settings.speed*(1.0/60.0));
    let current=log2(max(exposure[0],1e-4));
    exposure[0]=exp2(current+(log2(targetExposure)-current)*blend);
}
