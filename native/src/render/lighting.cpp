#include "apex/render/lighting.hpp"

#include "apex/core/parse_error.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <map>
#include <numbers>
#include <stdexcept>
#include <utility>

namespace apex::render {

namespace {

using apex::core::ParseError;
using Vec3 = LightingVec3;
using Mat4 = LightingMat4;
constexpr float pi = 3.14159265358979323846F;

[[nodiscard]] ParseError error(std::string_view source, std::string_view code, std::string_view message) {
    return ParseError("Lighting", std::string(source), 0, std::string(code), std::string(message));
}
[[nodiscard]] float finiteOr(float value, float fallback) noexcept { return std::isfinite(value) ? value : fallback; }
[[nodiscard]] float clamp(float value, float lo, float hi) noexcept { return std::max(lo, std::min(hi, value)); }
[[nodiscard]] float saturate(float value) noexcept { return clamp(finiteOr(value, 0), 0, 1); }
[[nodiscard]] Vec3 add(Vec3 a, Vec3 b) noexcept { return {a[0]+b[0],a[1]+b[1],a[2]+b[2]}; }
[[nodiscard]] Vec3 sub(Vec3 a, Vec3 b) noexcept { return {a[0]-b[0],a[1]-b[1],a[2]-b[2]}; }
[[nodiscard]] Vec3 scale(Vec3 a, float b) noexcept { return {a[0]*b,a[1]*b,a[2]*b}; }
[[nodiscard]] float dot(Vec3 a, Vec3 b) noexcept { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
[[nodiscard]] Vec3 cross(Vec3 a, Vec3 b) noexcept { return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]}; }
[[nodiscard]] float length(Vec3 a) noexcept { return std::hypot(std::hypot(a[0], a[1]), a[2]); }
[[nodiscard]] Vec3 normalize(Vec3 a) noexcept {
    const float size = length(a);
    return size > 1e-8F && std::isfinite(size) ? scale(a, 1.0F/size) : Vec3{0,1,0};
}
[[nodiscard]] Vec3 finiteVec3(Vec3 value, Vec3 fallback) noexcept {
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (!std::isfinite(value[index])) value[index] = fallback[index];
    }
    return value;
}
[[nodiscard]] bool finiteVec3(Vec3 value) noexcept {
    return std::all_of(value.begin(), value.end(), [](float component) { return std::isfinite(component); });
}
[[nodiscard]] Vec3 normalizeJs(Vec3 value) noexcept {
    const float size = length(value);
    return size > 1e-8F && std::isfinite(size) ? scale(value, 1.0F / size) : Vec3{};
}
[[nodiscard]] Vec3 mix(Vec3 high, Vec3 low, float amount) noexcept { return add(high, scale(sub(low, high), amount)); }
[[nodiscard]] Mat4 multiply(const Mat4& a, const Mat4& b) noexcept {
    Mat4 out{};
    for (std::size_t column=0; column<4; ++column) for (std::size_t row=0; row<4; ++row)
        for (std::size_t index=0; index<4; ++index) out[column*4+row] += a[index*4+row]*b[column*4+index];
    return out;
}
[[nodiscard]] Mat4 lookAt(Vec3 eye, Vec3 target, Vec3 up) noexcept {
    const Vec3 z = normalize(sub(eye,target)), x = normalize(cross(up,z)), y = cross(z,x);
    return {x[0],y[0],z[0],0,x[1],y[1],z[1],0,x[2],y[2],z[2],0,-dot(x,eye),-dot(y,eye),-dot(z,eye),1};
}
[[nodiscard]] Mat4 orthographic(float left,float right,float bottom,float top,float near_plane,float far_plane) noexcept {
    const float lr=1/(left-right), bt=1/(bottom-top), nf=1/(near_plane-far_plane);
    return {-2*lr,0,0,0,0,-2*bt,0,0,0,0,2*nf,0,(left+right)*lr,(top+bottom)*bt,(far_plane+near_plane)*nf,1};
}
[[nodiscard]] Mat4 perspective(float fov,float aspect,float near_plane,float far_plane) noexcept {
    const float f=1/std::tan(fov/2), nf=1/(near_plane-far_plane);
    return {f/aspect,0,0,0,0,f,0,0,0,0,(far_plane+near_plane)*nf,-1,0,0,2*far_plane*near_plane*nf,0};
}
[[nodiscard]] Vec3 transformPoint(const Mat4& m, Vec3 p) noexcept {
    return {m[0]*p[0]+m[4]*p[1]+m[8]*p[2]+m[12],m[1]*p[0]+m[5]*p[1]+m[9]*p[2]+m[13],m[2]*p[0]+m[6]*p[1]+m[10]*p[2]+m[14]};
}
[[nodiscard]] Vec3 average(std::span<const Vec3> values) noexcept {
    Vec3 out{}; for (const auto value: values) out=add(out,value); return values.empty()?out:scale(out,1.0F/static_cast<float>(values.size()));
}

struct Ini { std::map<std::string,std::string> values; };
[[nodiscard]] std::string upper(std::string value) {
    for (char& character : value) {
        character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
    }
    return value;
}
[[nodiscard]] Ini parseIni(std::string_view text, std::string_view source, const apex::core::ParseLimits& limits) {
    if (text.size()>limits.maxInputBytes) throw error(source,"input_too_large","lighting INI exceeds input limit");
    Ini ini; std::string section; std::size_t start=0, lines=0;
    while (start<=text.size()) {
        if (++lines>limits.maxTracks) throw error(source,"count_limit","lighting INI has too many lines");
        const auto end=text.find('\n',start);
        const auto raw_length=end==std::string_view::npos?text.size()-start:end-start;
        if (raw_length>limits.maxStringBytes) throw error(source,"string_limit","lighting INI line exceeds string limit");
        std::string line(text.substr(start,raw_length));
        if (!line.empty()&&line.back()=='\r') {
            line.pop_back();
        }
        if (const auto comment=line.find(';');comment!=std::string::npos) {
            line.resize(comment);
        }
        const auto first=line.find_first_not_of(" \t"), last=line.find_last_not_of(" \t"); line=first==std::string::npos?std::string{}:line.substr(first,last-first+1);
        if (!line.empty()&&line.front()=='['&&line.back()==']') {
            section=upper(line.substr(1,line.size()-2));
        } else if (!section.empty()) {
            const auto equal=line.find('=');
            if(equal!=std::string::npos) {
                auto key=line.substr(0,equal), value=line.substr(equal+1);
                if (key.size()>limits.maxStringBytes || value.size()>limits.maxStringBytes) {
                    throw error(source,"string_limit","lighting INI key or value exceeds string limit");
                }
                const auto k=key.find_first_not_of(" \t"), kl=key.find_last_not_of(" \t"), v=value.find_first_not_of(" \t"), vl=value.find_last_not_of(" \t");
                key=k==std::string::npos?std::string{}:key.substr(k,kl-k+1);
                value=v==std::string::npos?std::string{}:value.substr(v,vl-v+1);
                if(!key.empty()) ini.values[section+"\n"+upper(key)]=std::move(value);
            }
        }
        if(end==std::string_view::npos) {
            break;
        }
        start=end+1;
    }
    return ini;
}
[[nodiscard]] std::string raw(const Ini& ini,std::string_view section,std::string_view key) { const auto it=ini.values.find(std::string(section)+"\n"+std::string(key)); return it==ini.values.end()?std::string{}:it->second; }
[[nodiscard]] bool parseFloatToken(std::string_view token, float& value) {
    const std::string text(token);
    char* end=nullptr;
    value=std::strtof(text.c_str(),&end);
    if(end==text.c_str()||end==nullptr||!std::isfinite(value)) return false;
    while(*end!='\0'&&std::isspace(static_cast<unsigned char>(*end))!=0) ++end;
    return *end=='\0';
}
[[nodiscard]] float scalar(const Ini& ini,std::string_view section,std::string_view key,float fallback) { const auto text=raw(ini,section,key); if(text.empty()) return fallback; float value=0; return parseFloatToken(text,value)?value:fallback; }
[[nodiscard]] Vec3 vector3(const Ini& ini,std::string_view section,std::string_view key,std::string_view source) {
    const auto text=raw(ini,section,key); Vec3 out{}; std::size_t start=0; std::size_t count=0;
    while(start<=text.size()&&count<3) {
        const auto end=text.find(',',start);
        const auto token=text.substr(start,end==std::string_view::npos?text.size()-start:end-start);
        float value=0;
        if(!parseFloatToken(token,value)) {
            throw error(source,"invalid_value","invalid lighting vector");
        }
        out[count++]=value;
        if(end==std::string_view::npos) break;
        start=end+1;
    }
    if(count<3) {
        throw error(source,"missing_value","lighting vector has fewer than three components");
    }
    return out;
}
[[nodiscard]] Vec3 colorCurve(const Ini& ini,std::string_view section,std::string_view key,std::string_view source) {
    const auto text=raw(ini,section,key); std::array<float,4> values{}; std::size_t start=0,count=0;
    while(start<=text.size()&&count<4) {
        const auto end=text.find(',',start);
        const auto token=text.substr(start,end==std::string_view::npos?text.size()-start:end-start);
        float value=0;
        if(!parseFloatToken(token,value)) {
            throw error(source,"invalid_value","invalid color curve");
        }
        values[count++]=value;
        if(end==std::string_view::npos) break;
        start=end+1;
    }
    if(count<4) {
        throw error(source,"missing_value","color curve has fewer than four components");
    }
    return {values[0]*values[3]/255,values[1]*values[3]/255,values[2]*values[3]/255};
}
[[nodiscard]] WeatherPreset makePreset(std::string id,std::string name,float gamma,std::array<float,4> hl,std::array<float,4> hh,std::array<float,4> sl,std::array<float,4> sh,std::array<float,4> sul,std::array<float,4> suh,std::array<float,4> al,std::array<float,4> ah,Vec3 fog,float fog_blend,float fog_distance,float cover,float cutoff,float cloud_color,float width,float height,float radius,std::uint32_t number,float speed) {
    const auto curve=[](std::array<float,4> v)->Vec3{return {v[0]*v[3]/255,v[1]*v[3]/255,v[2]*v[3]/255};}; WeatherPreset p; p.id=std::move(id);p.name=std::move(name);p.source="Assetto Corsa SDK · "+p.id;p.angle_gamma=gamma;p.horizon_low=curve(hl);p.horizon_high=curve(hh);p.sky_low=curve(sl);p.sky_high=curve(sh);p.sun_low=curve(sul);p.sun_high=curve(suh);p.ambient_low=curve(al);p.ambient_high=curve(ah);p.fog_color=fog;p.fog_blend=fog_blend;p.fog_distance=fog_distance;p.cloud_cover=cover;p.cloud_cutoff=cutoff;p.cloud_color=cloud_color;p.cloud_width=width;p.cloud_height=height;p.cloud_radius=radius;p.cloud_number=number;p.cloud_base_speed=speed;return p;
}

} // namespace

const std::array<WeatherPreset,7>& stockWeatherPresets() noexcept {
    static const std::array<WeatherPreset,7> presets={
        makePreset("1_heavy_fog","Heavy Fog",2,{164,164,164,3.5F},{164,164,164,4},{164,164,164,3},{164,164,164,3.5F},{229.5F,168.3F,86.7F,0},{170,170,160,0},{160,150,140,10},{140,150,155,10.8F},{4.25F,4.25F,4.25F},1,2000,0,.69F,.3F,16,7,10,0,.0015F),
        makePreset("2_light_fog","Light Fog",1.8F,{150,150,150,5.25F},{150,150,150,5},{100,130,150,3.5F},{100,130,150,5.5F},{200.5F,110.3F,46.7F,9},{170,165,160,8},{130,120,105,10},{130,135,140,10},{4.15F,4.15F,4.15F},.85F,2700,0,.69F,.3F,16,7,10,0,.0015F),
        makePreset("3_clear","Clear",3.4F,{255,138,34,1.9F},{150,170,220,3.5F},{30,73,167,2.8F},{30,73,167,3},{229.5F,140,70,40},{170,160,140,20},{124,124,124,18},{105,105,105,11},{1.8F,2.37F,3.42F},.85F,9000,.9F,.5F,.7F,6,2.5F,6,0,.0015F),
        makePreset("4_mid_clear","Mid Clear",3.4F,{255,138,34,1.9F},{150,170,220,3.5F},{30,73,167,2.8F},{30,73,167,3},{229.5F,140,70,40},{170,160,140,20},{124,124,124,18},{105,105,105,11},{1.8F,2.37F,3.42F},.85F,9000,.9F,.5F,.7F,9,4,6,40,.0015F),
        makePreset("5_light_clouds","Light Clouds",1.8F,{255,138,34,5.5F},{140,170,200,5.5F},{100,133,187,6},{80,133,200,6.5F},{229.5F,168.3F,86.7F,6},{170,170,160,6},{135,120,118,11},{124,134,145,15},{1.5F,2.25F,3.5F},.8F,12000,1,.7F,3.05F,18,9,10,50,.004F),
        makePreset("6_mid_clouds","Mid Clouds",2.8F,{150,150,150,1.5F},{150,150,150,1.5F},{145,140,140,3.5F},{120,150,180,3.5F},{0,0,0,0},{0,0,0,0},{140,140,140,9},{140,140,140,11},{1.45F,1.55F,1.65F},.8F,12000,.4F,.85F,1.5F,25,12,10,45,.004F),
        makePreset("7_heavy_clouds","Heavy Clouds",2,{140,140,140,2},{140,140,140,2},{145,140,142,4},{140,140,140,4},{0,0,0,0},{0,0,0,0},{140,130,120,6},{140,140,140,8},{1.1F,1.1F,1.1F},.8F,12000,.95F,.55F,.2F,13,7,10,50,.004F)};
    return presets;
}
const WeatherPreset& defaultWeatherPreset() noexcept { return stockWeatherPresets()[4]; }

WeatherPreset parseKsWeatherLighting(std::string_view curves_text,std::string_view weather_text,std::string source,apex::core::ParseLimits limits) {
    if(source.size()>limits.maxStringBytes) throw error(source,"string_limit","lighting source name is too long");
    const Ini curves=parseIni(curves_text,source,limits), weather=parseIni(weather_text,source,limits);
    const float version=scalar(curves,"HEADER","VERSION",0); if(version!=3) throw error(source,"unsupported_version","colorCurves.ini version is not 3");
    WeatherPreset p; p.id=source;p.name=raw(weather,"LAUNCHER","NAME"); if(p.name.empty()) p.name=source;p.source=source;p.angle_gamma=scalar(curves,"HEADER","ANGLE_GAMMA",1);p.hdr_off_mult=scalar(curves,"HEADER","HDR_OFF_MULT",1);
    p.horizon_low=colorCurve(curves,"HORIZON","LOW",source);p.horizon_high=colorCurve(curves,"HORIZON","HIGH",source);p.sky_low=colorCurve(curves,"SKY","LOW",source);p.sky_high=colorCurve(curves,"SKY","HIGH",source);p.sun_low=colorCurve(curves,"SUN","LOW",source);p.sun_high=colorCurve(curves,"SUN","HIGH",source);p.ambient_low=colorCurve(curves,"AMBIENT","LOW",source);p.ambient_high=colorCurve(curves,"AMBIENT","HIGH",source);p.fog_color=vector3(weather,"FOG","COLOR",source);p.fog_blend=scalar(weather,"FOG","BLEND",1);p.fog_distance=std::max(1.0F,scalar(weather,"FOG","DISTANCE",1));p.cloud_cover=scalar(weather,"CLOUDS","COVER",0);p.cloud_cutoff=scalar(weather,"CLOUDS","CUTOFF",0);p.cloud_color=scalar(weather,"CLOUDS","COLOR",0);p.cloud_width=clamp(scalar(weather,"CLOUDS","WIDTH",4),0,1000);p.cloud_height=clamp(scalar(weather,"CLOUDS","HEIGHT",2),0,1000);p.cloud_radius=clamp(scalar(weather,"CLOUDS","RADIUS",4),0,1000);p.cloud_number=static_cast<std::uint32_t>(clamp(std::floor(scalar(weather,"CLOUDS","NUMBER",100)),0,512));p.cloud_base_speed=clamp(scalar(weather,"CLOUDS","BASE_SPEED_MULT",.01F),0,1); return p;
}

EvaluatedLighting evaluateKsLighting(const WeatherPreset& p,Vec3 sun_direction) { EvaluatedLighting out;out.preset=p;out.sun_direction=normalizeJs(sun_direction);out.sun_height=clamp(out.sun_direction[1],0,1);out.angle_mix=std::pow(1-out.sun_height,std::max(.001F,p.angle_gamma));out.horizon_color=mix(p.horizon_high,p.horizon_low,out.angle_mix);out.sky_color=mix(p.sky_high,p.sky_low,out.angle_mix);out.sun_color=mix(p.sun_high,p.sun_low,out.angle_mix);out.ambient_color=mix(p.ambient_high,p.ambient_low,out.angle_mix);out.fog_color=p.fog_color;out.fog_blend=p.fog_blend;out.fog_distance=p.fog_distance;out.source_kind=p.source_kind;return out; }
LightingVec3 sunDirectionFromAngles(float heading,float height) noexcept { const float h=clamp(finiteOr(height,0),0,90)*pi/180, heading_r=finiteOr(heading,0)*pi/180, horizontal=std::cos(h);return normalize({std::sin(heading_r)*horizontal,std::sin(h),std::cos(heading_r)*horizontal}); }

float ksEditorAutoExposure(float luminance,float target,float minimum,float maximum) noexcept { const float measured=std::max(.0001F,finiteOr(luminance,0)), low=std::max(0.0F,finiteOr(minimum,0)), high=std::max(low,finiteOr(maximum,0));return clamp(finiteOr(target,0)/measured,low,high); }
LightingVec3 ksEditorYebisToneMap(Vec3 rgb,float exposure,float gamma,float saturation,float curve_scale,float curve_shoulder) noexcept { const Vec3 source={std::max(0.0F,finiteOr(rgb[0],0)),std::max(0.0F,finiteOr(rgb[1],0)),std::max(0.0F,finiteOr(rgb[2],0))};const float luminance=source[0]*.2126F+source[1]*.7152F+source[2]*.0722F, gain=std::max(0.0F,finiteOr(exposure,0)), sat=finiteOr(saturation,0);float gamma_value=finiteOr(gamma,1);if(gamma_value==0) gamma_value=1;const float exponent=1/std::max(std::numeric_limits<float>::epsilon(),gamma_value);Vec3 out{};for(std::size_t i=0;i<3;++i){const float value=std::max(ks_editor_tonemap_input_floor,(luminance+(source[i]-luminance)*sat)*gain),decay=std::exp(-value*finiteOr(curve_scale,0)),shoulder=1-decay*finiteOr(curve_shoulder,0),curve=clamp((1-decay)*shoulder*shoulder,0,1);out[i]=std::pow(std::min(1.0F,curve+ks_editor_tonemap_output_epsilon),exponent);}return out; }
LightingVec3 ksEditorGlareBrightPass(Vec3 rgb,float exposure,float threshold,float remap) noexcept { Vec3 out{};const float gain=std::max(0.0F,finiteOr(exposure,0)), cutoff=std::max(0.0F,finiteOr(threshold,0)), scale_v=std::max(0.0F,finiteOr(remap,0));for(std::size_t i=0;i<3;++i)out[i]=clamp((std::max(0.0F,finiteOr(rgb[i],0))*gain-cutoff)*scale_v,0,64000);return out; }
float ksEditorBloomCompositeScale(float range,float glare,float shape,float bloom) noexcept { return std::max(0.0F,finiteOr(range,0))*.035F*std::max(0.0F,finiteOr(shape,0))*std::max(0.0F,finiteOr(glare,0))*std::max(0.0F,finiteOr(bloom,0)); }

BloomKernelMetadata ksEditorBloomGaussianKernel(std::int32_t level,float threshold,float radius_scale,std::int32_t source_level,float display_scale,std::array<float,4> dispersion) noexcept { const float safe_level=static_cast<float>(std::max(0,level)), safe_source=static_cast<float>(std::max(0,source_level)), sigma=std::pow(2.0F,safe_level)*std::pow(2.0F,1-safe_source)*std::max(0.0F,finiteOr(radius_scale,0))*std::max(0.0F,finiteOr(display_scale,0));BloomKernelMetadata out;out.sigma=sigma;for(std::size_t c=0;c<4;++c)out.channel_sigmas[c]=sigma*std::max(0.0F,finiteOr(dispersion[c],0));std::array<std::array<float,15>,4> offsets{},weights{};for(std::size_t c=0;c<4;++c){const float radius=std::max(1e-8F,out.channel_sigmas[c]);std::array<float,15> gaussian{};for(std::size_t i=0;i<15;++i)gaussian[i]=std::exp(-static_cast<float>(i*i)/(2*radius*radius));float total=gaussian[0];for(std::size_t i=1;i<15;++i)total+=2*gaussian[i];offsets[c][0]=0;weights[c][0]=gaussian[0]/total;for(std::size_t pair=0;pair<7;++pair){const std::size_t odd=pair*2+1, even=odd+1;const float pair_weight=gaussian[odd]+gaussian[even], offset=pair_weight>0?(gaussian[odd]*static_cast<float>(odd)+gaussian[even]*static_cast<float>(even))/pair_weight:static_cast<float>(even);offsets[c][pair*2+1]=offset;offsets[c][pair*2+2]=-offset;weights[c][pair*2+1]=pair_weight/total;weights[c][pair*2+2]=pair_weight/total;}}for(std::size_t i=0;i<15;++i)for(std::size_t c=0;c<4;++c)out.offsets[i]+=offsets[c][i]*.25F;out.weights=weights[0];float cutoff=15;for(std::size_t tap=2;tap<15;++tap){const float safe_sigma=std::max(1e-8F,sigma);const float normalizer=std::sqrt(2*pi)*safe_sigma,density=std::exp(-static_cast<float>(tap*tap)/(2*safe_sigma*safe_sigma))/normalizer;if(density<std::max(0.0F,finiteOr(threshold,0))){cutoff=static_cast<float>(tap);break;}}out.sample_count=std::min<std::uint32_t>(15,static_cast<std::uint32_t>(std::floor(cutoff/2)*2+1));return out; }

LineLightSample cspLineLightSample(Vec3 from,Vec3 to,Vec3 position,Vec3 color_from,Vec3 color_to) noexcept {
    if (!finiteVec3(from) || !finiteVec3(to) || !finiteVec3(position)) return {};
    color_from=finiteVec3(color_from,{}); color_to=finiteVec3(color_to,{});
    const Vec3 ab=sub(to,from), delta=sub(position,from);const float len2=dot(ab,ab), inverse=len2>1e-12F?1/len2:0,value=inverse*dot(delta,ab), t=clamp(value,0,1);LineLightSample out;out.value=value;out.clamped_value=t;out.distance_inverse=inverse;out.point=add(from,scale(ab,t));out.color=add(color_from,scale(sub(color_to,color_from),t));return out;
}
bool cspLightReceiverVisible(std::string_view view_mode,bool interior,std::string_view track_mode,bool track_receiver) noexcept { if(view_mode=="interior"&&!interior)return false;if(view_mode=="exterior"&&interior)return false;if(track_receiver&&(track_mode=="none"||(track_mode=="interior-only"&&!interior)))return false;return true; }
float cspLightDistanceFade(float distance,float fade_at,float fade_smooth) noexcept { const float center=std::max(0.0F,finiteOr(fade_at,0.0F)),width=std::max(0.0F,finiteOr(fade_smooth,0.0F)),value=std::max(0.0F,finiteOr(distance,0.0F));if(width<=1e-6F)return value<center?1.0F:0.0F;return clamp((center+width*.5F-value)/width,0.0F,1.0F); }
SpotConePacking cspSpotConePacking(Vec3 direction,float spot_degrees,float sharpness) noexcept { SpotConePacking out;const float size=length(direction),spot=finiteOr(spot_degrees,0);if(!finiteVec3(direction)||!(size>1e-8F)||!(spot>0))return out;out.enabled=true;out.half_angle=std::max(.01F,std::min(3.1414182F,spot*.017453294F))*.5F;out.sharpness=clamp(finiteOr(sharpness,0),0,.999F);out.outer_cos=std::cos(out.half_angle);out.inner_cos=std::cos(out.sharpness*out.half_angle);out.inverse_width=1/(out.outer_cos-out.inner_cos);out.direction=scale(direction,1/size*out.inverse_width);out.start=out.outer_cos*out.inverse_width;return out; }
float cspSpotConeFactor(Vec3 direction,Vec3 to_light,float spot,float sharpness) noexcept { const auto packed=cspSpotConePacking(direction,spot,sharpness);if(!packed.enabled)return 1;if(!finiteVec3(to_light))return 0;const float size=length(to_light);if(!(size>1e-8F))return 0;return clamp(packed.start-dot(packed.direction,scale(to_light,-1/size)),0,1); }
SpotEdgePacking cspSpotEdgePacking(Vec3 up,Vec3 edge,float sharpness) noexcept { SpotEdgePacking out;const float size=length(up),amount=std::max(0.0F,finiteOr(sharpness,0));if(!finiteVec3(up)||!finiteVec3(edge)||!(amount>0)||!(size>1e-8F))return out;out.enabled=true;out.sharpness=amount;out.up=scale(up,amount/size);out.offsets=scale(edge,amount);return out; }
LightingVec3 cspSpotEdgeFactors(Vec3 up,Vec3 edge,float sharpness,Vec3 to_light) noexcept { const auto packed=cspSpotEdgePacking(up,edge,sharpness);if(!packed.enabled)return {1,1,1};if(!finiteVec3(to_light))return {0,0,0};const float size=length(to_light);if(!(size>1e-8F))return {0,0,0};const float projected=dot(packed.up,scale(to_light,-1/size));return {clamp(packed.offsets[0]-projected,0,1),clamp(packed.offsets[1]-projected,0,1),clamp(packed.offsets[2]-projected,0,1)}; }
SecondarySpotPacking cspSecondarySpotPacking(float range,float skip) noexcept { SecondarySpotPacking out;out.range_inverse=1/std::max(1e-8F,finiteOr(range,0));out.skip=clamp(finiteOr(skip,0),0,1);if(!(range>0)&&!(out.skip>0)){out.range_inverse=0;out.skip=0;return out;}if(!(range>0)||!(out.skip>0)){out.range_inverse=0;out.skip=std::max(0.0F,out.skip);return out;}out.enabled=true;out.trim_length_inverse=1/(range*out.skip);return out; }
float cspSecondarySpotAttenuation(float distance,float range,float skip) noexcept { const auto p=cspSecondarySpotPacking(range,skip);if(!p.enabled)return 0;const float value=std::max(0.0F,finiteOr(distance,0)), shaped=std::max(0.0F,std::min(1.0F,value*p.trim_length_inverse)-std::min(1.0F,value*p.range_inverse));return shaped*shaped; }

ShadowFilter cspLocalShadowFilter(std::string_view section,bool extra,std::string_view source) noexcept { const auto prefix=[](std::string_view value,std::string_view wanted){if(value.size()<wanted.size()||!std::equal(value.begin(),value.begin()+wanted.size(),wanted.begin(),[](char a,char b){return std::tolower(static_cast<unsigned char>(a))==std::tolower(static_cast<unsigned char>(b));}))return false;return value.size()==wanted.size()||value[wanted.size()]=='_';};const auto containsInsensitive=[](std::string_view value,std::string_view wanted){if(wanted.empty())return true;for(std::size_t start=0;start+wanted.size()<=value.size();++start)if(std::equal(value.begin()+start,value.begin()+start+wanted.size(),wanted.begin(),[](char a,char b){return std::tolower(static_cast<unsigned char>(a))==std::tolower(static_cast<unsigned char>(b));}))return true;return false;};const bool head=prefix(section,"LIGHT_HEADLIGHT")||containsInsensitive(source,"SELFLIGHT_HEADLIGHTS");ShadowFilter out;out.mode=head?2:extra?1:0;out.kernel=head||!extra?7:15;out.weights=head||!extra?std::array<float,4>{.4490798F,.0509202F,0,0}:std::array<float,4>{.2496147F,.1924633F,.0514763F,.0064457F};out.offsets=head||!extra?std::array<float,4>{.5380487F,2.0627797F,0,0}:std::array<float,4>{.6443417F,2.3788476F,4.2911105F,6.2166071F};out.value_aware=head;return out; }
ExponentialShadowParams cspExponentialShadowParams(const ExponentialShadowInput& i) noexcept { const float raw_range=std::isfinite(i.range)&&i.range!=0.0F?i.range:10.0F,source=std::max(.001F,raw_range),limit=i.vehicle_attached?120.0F:30.0F,max_range=i.shadow_range_authored&&i.shadow_range>0.0F?finiteOr(i.shadow_range,source):std::min(source,limit),raw_exp=std::isfinite(i.exp_factor)&&i.exp_factor!=0.0F?i.exp_factor:20.0F,exp_factor=clamp(raw_exp,.001F,80.0F),spot=std::max(0.0F,finiteOr(i.shadow_spot,0.0F))*pi/180.0F,automatic=10.0F-7.5F*clamp((spot-pi*.6F)/(pi*.3F),0.0F,1.0F),boost=i.shadow_boost>0.0F?finiteOr(i.shadow_boost,automatic):automatic;ExponentialShadowParams o;o.max_range=max_range;o.range_inverse=1.0F/max_range;o.range_inverse_exp_factor=exp_factor/max_range;o.exp_factor=exp_factor;o.bias_mult=exp_factor*.3F/max_range;o.thickness_fix_mult=boost;o.thickness_fix_add=1.0F-boost;o.boost=boost;o.clip_plane=std::max(.001F,std::isfinite(i.shadow_clip_plane)&&i.shadow_clip_plane>0.0F?i.shadow_clip_plane:.5F);o.clip_sphere=std::max(0.0F,i.shadow_clip_sphere_authored&&std::isfinite(i.shadow_clip_sphere)?i.shadow_clip_sphere:.5F);o.extra_blur=i.shadow_extra_blur;return o; }
float resolveCspExponentialShadow(float occluder,float receiver,float bias,const ExponentialShadowParams& p) noexcept { return clamp(occluder*std::exp(p.bias_mult*bias-std::min(receiver,p.max_range)*p.range_inverse_exp_factor)*p.thickness_fix_mult+p.thickness_fix_add,0,1); }

DirectionalShadowResult computeDirectionalShadowCascades(const DirectionalShadowInput& i) { const Vec3 eye=finiteVec3(i.eye,{}),target=finiteVec3(i.target,{0,0,-1}),up=finiteVec3(i.up,{0,1,0});const Vec3 forward=normalize(sub(target,eye));Vec3 right=normalize(cross(forward,up));if(length(right)<.5F)right=normalize(cross(forward,{0,0,1}));const Vec3 camera_up=normalize(cross(right,forward)),light=normalize(finiteVec3(i.sun_direction,ks_sun_direction));const float near_plane=std::max(.001F,finiteOr(i.near_plane,.001F)),split_fallback=finiteOr(i.splits.back(),50),far_input=std::isfinite(i.far_plane)&&i.far_plane!=0?i.far_plane:split_fallback,far_plane=std::max(near_plane+.001F,far_input);const float tan_half=std::tan(std::max(.001F,finiteOr(i.fov_radians,.785398F))*.5F),padding=clamp(std::max(1.0F,finiteOr(i.scene_radius,1))*2,50,500);DirectionalShadowResult out;out.forward=forward;out.light_direction=light;out.map_size=std::min(16384U,std::max(1U,i.map_size));float previous=near_plane;for(std::size_t index=0;index<3;++index){const float split=std::max(previous+.001F,std::min(far_plane,finiteOr(i.splits[index],far_plane)));std::vector<Vec3> corners;corners.reserve(8);for(float depth:{previous,split}){const float hh=depth*tan_half,hw=hh*std::max(.001F,finiteOr(i.aspect,1));const Vec3 c=add(eye,scale(forward,depth));for(float v:{-1.0F,1.0F})for(float h:{-1.0F,1.0F})corners.push_back(add(add(c,scale(right,hw*h)),scale(camera_up,hh*v)));}const Vec3 center=average(corners);float radius=.01F;for(const auto point:corners)radius=std::max(radius,length(sub(point,center)));const Vec3 light_eye=add(center,scale(light,radius+padding));const Mat4 light_view=lookAt(light_eye,center,std::abs(light[1])>.98F?Vec3{0,0,1}:Vec3{0,1,0});std::vector<Vec3> lc;for(const auto point:corners)lc.push_back(transformPoint(light_view,point));Vec3 minimum{std::numeric_limits<float>::max(),std::numeric_limits<float>::max(),std::numeric_limits<float>::max()},maximum{std::numeric_limits<float>::lowest(),std::numeric_limits<float>::lowest(),std::numeric_limits<float>::lowest()};for(const auto point:lc)for(std::size_t axis=0;axis<3;++axis){minimum[axis]=std::min(minimum[axis],point[axis]);maximum[axis]=std::max(maximum[axis],point[axis]);}const float extent=std::max(maximum[0]-minimum[0],maximum[1]-minimum[1])*1.04F,texel=extent/static_cast<float>(out.map_size),cx=std::round((minimum[0]+maximum[0])*.5F/texel)*texel,cy=std::round((minimum[1]+maximum[1])*.5F/texel)*texel,half=extent*.5F;out.cascades.push_back({static_cast<std::uint32_t>(index),previous,split,radius,texel,center,multiply(orthographic(cx-half,cx+half,cy-half,cy+half,-maximum[2]-padding,-minimum[2]+padding),light_view)});out.splits.push_back(split);previous=split;}return out; }
DirectionalShadowResult computeDirectionalProbeShadowCascades(const ProbeShadowInput& i) { DirectionalShadowResult out;out.light_direction=normalize(finiteVec3(i.sun_direction,ks_sun_direction));out.map_size=std::min(16384U,std::max(1U,i.map_size));const Vec3 center=finiteVec3(i.eye,{});const float padding=clamp(std::max(1.0F,finiteOr(i.scene_radius,1))*2,50,500);float previous=.001F;for(std::size_t index=0;index<3;++index){const float radius=std::max(previous+.001F,finiteOr(i.splits[index],previous+1));std::vector<Vec3> corners;for(float x:{-radius,radius})for(float y:{-radius,radius})for(float z:{-radius,radius})corners.push_back(add(center,{x,y,z}));const Vec3 light_eye=add(center,scale(out.light_direction,radius+padding));const Mat4 view=lookAt(light_eye,center,std::abs(out.light_direction[1])>.98F?Vec3{0,0,1}:Vec3{0,1,0});Vec3 minimum{std::numeric_limits<float>::max(),std::numeric_limits<float>::max(),std::numeric_limits<float>::max()},maximum{std::numeric_limits<float>::lowest(),std::numeric_limits<float>::lowest(),std::numeric_limits<float>::lowest()};for(const auto point:corners){const auto p=transformPoint(view,point);for(std::size_t axis=0;axis<3;++axis){minimum[axis]=std::min(minimum[axis],p[axis]);maximum[axis]=std::max(maximum[axis],p[axis]);}}const float extent=std::max(maximum[0]-minimum[0],maximum[1]-minimum[1])*1.04F,texel=extent/static_cast<float>(out.map_size),cx=std::round((minimum[0]+maximum[0])*.5F/texel)*texel,cy=std::round((minimum[1]+maximum[1])*.5F/texel)*texel,half=extent*.5F;out.cascades.push_back({static_cast<std::uint32_t>(index),previous,radius,radius,texel,center,multiply(orthographic(cx-half,cx+half,cy-half,cy+half,-maximum[2]-padding,-minimum[2]+padding),view)});out.splits.push_back(radius);previous=radius;}return out; }
LocalShadowResult computeLocalLightShadow(const LocalShadowInput& i) { auto esm_input=i.esm;esm_input.range=i.range;if(!esm_input.shadow_spot_authored&&!(esm_input.shadow_spot>0))esm_input.shadow_spot=i.spot;const auto esm=cspExponentialShadowParams(esm_input);const Vec3 position=finiteVec3(i.position,{}),raw_direction=finiteVec3(i.direction,{0,-1,0});const Vec3 direction=length(raw_direction)>1e-8F?normalize(raw_direction):Vec3{0,-1,0};const float spot=clamp(finiteOr(i.spot,90),1,175),near_plane=esm.clip_plane,far_plane=std::max(near_plane+.001F,esm.max_range);return {multiply(perspective(spot*pi/180,1,near_plane,far_plane),lookAt(position,add(position,direction),std::abs(direction[1])>.98F?Vec3{0,0,1}:Vec3{0,1,0})),position,direction,spot,near_plane,far_plane,esm,LightingSource::source_evidenced}; }
bool shadowCasterEnabled(bool node,std::optional<bool> override_value) noexcept { return override_value.value_or(node); }

const CubemapConfig& ksEditorCubemap() noexcept { static const CubemapConfig config{};return config; }
const std::array<CubemapFace,6>& webglCubemapFaces() noexcept { static const std::array<CubemapFace,6> faces={CubemapFace{{-1,0,0},{0,-1,0},"negative-x"},{{1,0,0},{0,-1,0},"positive-x"},{{0,1,0},{0,0,1},"positive-y"},{{0,-1,0},{0,0,-1},"negative-y"},{{0,0,1},{0,-1,0},"positive-z"},{{0,0,-1},{0,-1,0},"negative-z"}};return faces; }
ReflectionCaptureSelection selectReflectionCaptureItems(std::span<const ReflectionCaptureItem> items,std::string_view explicit_root,std::string_view workspace_kind,float bounds_radius,bool isolated) { ReflectionCaptureSelection out;if(isolated){out.mode=ReflectionCaptureMode::disabled;out.reason="Isolated mesh preview";return out;}if(!explicit_root.empty()){out.mode=ReflectionCaptureMode::explicit_subtree;out.root_name=std::string(explicit_root);for(std::size_t i=0;i<items.size();++i)if(items[i].explicit_member)out.item_indices.push_back(i);if(out.item_indices.empty())out.reason="Selected reflection subtree has no visible geometry";return out;}for(std::size_t i=0;i<items.size();++i)if(items[i].environment){out.item_indices.push_back(i);if(out.root_name.empty())out.root_name=items[i].name.empty()?"Reflection environment":items[i].name;}if(!out.item_indices.empty()){out.mode=ReflectionCaptureMode::environment;return out;}if(workspace_kind=="track"||finiteOr(bounds_radius,0)>20){out.mode=ReflectionCaptureMode::scene;out.root_name="Whole scene";for(std::size_t i=0;i<items.size();++i)out.item_indices.push_back(i);if(out.item_indices.empty())out.reason="Scene has no visible geometry";return out;}out.source_kind=LightingSource::procedural_fallback;out.reason="No separate environment geometry";return out; }
float reflectionSmoothMinimum(float a,float b) noexcept { const float first=finiteOr(a,0),second=finiteOr(b,0),h=saturate(second-std::abs(first-second));return std::min(first,second)-h*h/std::max(.00001F,second*3)*(1-second); }
float reflectionFresnel(float facing,float c,float exponent,float maximum,float multiplier) noexcept { const float angle=std::pow(saturate(facing),std::max(.01F,finiteOr(exponent,0)));return std::max(0.0F,reflectionSmoothMinimum(angle+std::max(0.0F,finiteOr(c,0)),std::max(0.0F,finiteOr(maximum,0))))*std::max(0.0F,finiteOr(multiplier,0)); }
ReflectionBlur reflectionBlurFromExponent(float exponent) noexcept { const float base=saturate(1-std::max(0.0F,finiteOr(exponent,0))/255);return {base*6,base*base*6}; }
ReflectionFallback portableReflectionEnvironment(Vec3 direction,float blur,Vec3 sky,Vec3 horizon,Vec3 fog,float fog_blend) noexcept { const Vec3 d=normalizeJs(direction);const float y=d[1],upper=saturate(y),horizon_weight=(1-upper)*(1-upper),fog_mix=saturate(fog_blend)*.35F,ground_edge=.025F+(.24F-.025F)*saturate(blur/6),edge_t=saturate((y+ground_edge)/(ground_edge*2)),smooth=edge_t*edge_t*(3-2*edge_t);Vec3 upper_color{};for(std::size_t i=0;i<3;++i)upper_color[i]=(finiteOr(sky[i],0)*(1-horizon_weight)+finiteOr(horizon[i],0)*horizon_weight)*saturate(.75F+y*2);for(std::size_t i=0;i<3;++i)upper_color[i]=upper_color[i]*(1-fog_mix)+finiteOr(fog[i],0)*fog_mix;const Vec3 ground{.05F,.11F,.08F};Vec3 out{};for(std::size_t i=0;i<3;++i)out[i]=(ground[i]*(1-smooth)+upper_color[i]*smooth)*.12F;return {out,LightingSource::procedural_fallback}; }

} // namespace apex::render

namespace apex::render {

DirectionalShadowClipSpaceResult convertDirectionalShadowCascadeMatrix(
    const LightingMat4& webgl_matrix, CameraClipSpace native_clip_space) {
    if (native_clip_space != CameraClipSpace::vulkan &&
        native_clip_space != CameraClipSpace::d3d12) {
        return {DirectionalShadowClipSpaceStatus::unsupported, {},
                "directional_shadow_clip_space_unsupported",
                "Directional shadow conversion requires Vulkan or D3D12 clip space"};
    }
    if (!std::all_of(webgl_matrix.begin(), webgl_matrix.end(),
                     [](float value) { return std::isfinite(value); })) {
        return {DirectionalShadowClipSpaceStatus::invalid_input, {},
                "directional_shadow_matrix_non_finite",
                "Directional shadow cascade matrix must contain only finite values"};
    }
    LightingMat4 converted{};
    for (std::size_t column = 0U; column < 4U; ++column) {
        converted[column * 4U] = webgl_matrix[column * 4U];
        converted[column * 4U + 1U] =
            native_clip_space == CameraClipSpace::vulkan
                ? -webgl_matrix[column * 4U + 1U]
                : webgl_matrix[column * 4U + 1U];
        converted[column * 4U + 2U] =
            0.5F * webgl_matrix[column * 4U + 2U] +
            0.5F * webgl_matrix[column * 4U + 3U];
        converted[column * 4U + 3U] = webgl_matrix[column * 4U + 3U];
    }
    if (!std::all_of(converted.begin(), converted.end(),
                     [](float value) { return std::isfinite(value); })) {
        return {DirectionalShadowClipSpaceStatus::invalid_input, {},
                "directional_shadow_matrix_conversion_non_finite",
                "Directional shadow clip-space conversion exceeded finite float range"};
    }
    return {DirectionalShadowClipSpaceStatus::ready, converted, {}, {}};
}

} // namespace apex::render
