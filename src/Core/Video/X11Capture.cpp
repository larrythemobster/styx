// Ported from ouwou/Acheron video-streams commit 71b4907 (GPL-3.0-or-later).
#include "Core/Video/LinuxCapture.hpp"

#if defined(__linux__) && defined(STYX_HAVE_X11) && defined(STYX_HAVE_AVDEVICE)
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <unistd.h>
extern "C" {
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
}
#include <xcb/composite.h>
#include <xcb/randr.h>
#include <xcb/xcb.h>
#endif

namespace Styx::Core::Video::X11Capture {
#if defined(__linux__) && defined(STYX_HAVE_X11) && defined(STYX_HAVE_AVDEVICE)
namespace {
using Clock = std::chrono::steady_clock;
constexpr auto resizeSettle = std::chrono::milliseconds(300);
constexpr auto hiddenWait = std::chrono::milliseconds(50);
constexpr std::uint32_t normalState = 1, iconicState = 3, longestProperty = 4096;
struct Disconnect { void operator()(xcb_connection_t *c) const { if (c) xcb_disconnect(c); } };
struct Free { void operator()(void *p) const { std::free(p); } };
template <typename T> using Reply = std::unique_ptr<T, Free>;
struct InputCloser { void operator()(AVFormatContext *p) const { avformat_close_input(&p); } };
struct PacketCloser { void operator()(AVPacket *p) const { av_packet_free(&p); } };
struct Display {
 std::unique_ptr<xcb_connection_t, Disconnect> connection; xcb_window_t root = XCB_NONE;
 explicit operator bool() const { return root != XCB_NONE && connection && !xcb_connection_has_error(connection.get()); }
 operator xcb_connection_t*() const { return connection.get(); }
};
Display connectToDisplay() { Display d; int number = 0; d.connection.reset(xcb_connect(nullptr, &number)); if (!d.connection || xcb_connection_has_error(d.connection.get())) return d; auto screen=xcb_setup_roots_iterator(xcb_get_setup(d.connection.get())); for (;screen.rem>1 && number>0;--number) xcb_screen_next(&screen); if (screen.rem) d.root=screen.data->root; return d; }
bool has(const Display &d, xcb_extension_t *e) { const auto *r=xcb_get_extension_data(d,e); return r && r->present; }
xcb_atom_t atom(const Display &d,const char *name) { Reply<xcb_intern_atom_reply_t> r(xcb_intern_atom_reply(d,xcb_intern_atom(d,0,static_cast<std::uint16_t>(std::strlen(name)),name),nullptr)); return r?r->atom:XCB_ATOM_NONE; }
struct Atoms { explicit Atoms(const Display &d):stack(atom(d,"_NET_CLIENT_LIST_STACKING")),clients(atom(d,"_NET_CLIENT_LIST")),wmState(atom(d,"WM_STATE")),pid(atom(d,"_NET_WM_PID")),type(atom(d,"_NET_WM_WINDOW_TYPE")),normal(atom(d,"_NET_WM_WINDOW_TYPE_NORMAL")),dialog(atom(d,"_NET_WM_WINDOW_TYPE_DIALOG")),state(atom(d,"_NET_WM_STATE")),skip(atom(d,"_NET_WM_STATE_SKIP_TASKBAR")),name(atom(d,"_NET_WM_NAME")){} xcb_atom_t stack,clients,wmState,pid,type,normal,dialog,state,skip,name; };
Reply<xcb_get_property_reply_t> property(const Display &d,xcb_window_t w,xcb_atom_t a) { return Reply<xcb_get_property_reply_t>(xcb_get_property_reply(d,xcb_get_property(d,0,w,a,XCB_GET_PROPERTY_TYPE_ANY,0,longestProperty),nullptr)); }
std::vector<std::uint32_t> numbers(const Display &d,xcb_window_t w,xcb_atom_t a) { auto r=property(d,w,a); if(!r||r->format!=32) return {}; auto *v=static_cast<const std::uint32_t*>(xcb_get_property_value(r.get())); return {v,v+r->value_len}; }
std::string text(const Display &d,xcb_window_t w,xcb_atom_t a) { auto r=property(d,w,a); if(!r||r->format!=8) return {}; auto *v=static_cast<const char*>(xcb_get_property_value(r.get())); return {v,static_cast<std::size_t>(xcb_get_property_value_length(r.get()))}; }
bool holds(const std::vector<std::uint32_t>&v,std::uint32_t x) { return std::find(v.begin(),v.end(),x)!=v.end(); }
const AVInputFormat *grabDevice(){ static const auto *device=[] { avdevice_register_all(); return av_find_input_format("x11grab"); }(); return device; }
std::vector<CaptureRect> screenAreas(const Display &d) { std::vector<CaptureRect> r; if(has(d,&xcb_randr_id)){ Reply<xcb_randr_get_monitors_reply_t> a(xcb_randr_get_monitors_reply(d,xcb_randr_get_monitors(d,d.root,1),nullptr)); if(a) { std::vector<xcb_randr_monitor_info_t> m; for(auto i=xcb_randr_get_monitors_monitors_iterator(a.get());i.rem;xcb_randr_monitor_info_next(&i))m.push_back(*i.data); std::sort(m.begin(),m.end(),[](auto&a,auto&b){return std::tuple(!a.primary,a.x,a.y)<std::tuple(!b.primary,b.x,b.y);}); for(auto &x:m)r.push_back({x.x,x.y,x.width,x.height}); }} if(r.empty()){ Reply<xcb_get_geometry_reply_t> a(xcb_get_geometry_reply(d,xcb_get_geometry(d,d.root),nullptr)); if(a)r.push_back({0,0,a->width,a->height}); } return r; }
bool xwayland(const Display &d) { Reply<xcb_query_extension_reply_t> a(xcb_query_extension_reply(d,xcb_query_extension(d,8,"XWAYLAND"),nullptr)); return a&&a->present; }
xcb_window_t windowOf(const std::string&id) { if(!id.starts_with("window:")) return XCB_NONE; try { auto p=std::stoul(id.substr(7)); return p<=UINT32_MAX?static_cast<xcb_window_t>(p):XCB_NONE; } catch(...) { return XCB_NONE; } }
enum class Showing { Gone, Hidden, Shown };
Showing showing(const Display&d,xcb_window_t w){ Reply<xcb_get_window_attributes_reply_t> r(xcb_get_window_attributes_reply(d,xcb_get_window_attributes(d,w),nullptr)); return !r?Showing::Gone:r->map_state==XCB_MAP_STATE_VIEWABLE?Showing::Shown:Showing::Hidden; }
CaptureRect inside(const Display&d,xcb_window_t w){ Reply<xcb_get_geometry_reply_t> g(xcb_get_geometry_reply(d,xcb_get_geometry(d,w),nullptr)); if(!g)return{}; auto e=numbers(d,w,atom(d,"_GTK_FRAME_EXTENTS")); if(e.size()==4&&static_cast<std::uint64_t>(e[0])+e[1]<g->width&&static_cast<std::uint64_t>(e[2])+e[3]<g->height)return{static_cast<int>(e[0]),static_cast<int>(e[2]),static_cast<int>(g->width-e[0]-e[1]),static_cast<int>(g->height-e[2]-e[3])}; return{0,0,g->width,g->height}; }
CaptureRect insideOnRoot(const Display&d,xcb_window_t w){ Reply<xcb_translate_coordinates_reply_t> c(xcb_translate_coordinates_reply(d,xcb_translate_coordinates(d,w,d.root,0,0),nullptr)); auto r=inside(d,w); if(c){r.x+=c->dst_x;r.y+=c->dst_y;} return r; }
std::vector<xcb_window_t> children(const Display&d,xcb_window_t w){ Reply<xcb_query_tree_reply_t> r(xcb_query_tree_reply(d,xcb_query_tree(d,w),nullptr)); if(!r)return{}; auto*p=xcb_query_tree_children(r.get()); return{p,p+xcb_query_tree_children_length(r.get())}; }
xcb_window_t clientWithin(const Display&d,const Atoms&a,xcb_window_t w,int depth=4){if(!numbers(d,w,a.wmState).empty())return w;if(depth)for(auto c:children(d,w))if(auto r=clientWithin(d,a,c,depth-1))return r;return XCB_NONE;}
std::vector<xcb_window_t> topDown(const Display&d,const Atoms&a){auto r=numbers(d,d.root,a.stack);if(r.empty())r=numbers(d,d.root,a.clients);if(r.empty())for(auto c:children(d,d.root))if(auto x=clientWithin(d,a,c))r.push_back(x);std::reverse(r.begin(),r.end());return r;}
bool shareable(const Display&d,const Atoms&a,xcb_window_t w){auto s=numbers(d,w,a.wmState);if(s.empty()||(s[0]!=normalState&&s[0]!=iconicState)||holds(numbers(d,w,a.pid),static_cast<std::uint32_t>(getpid())))return false;auto t=numbers(d,w,a.type);return (t.empty()||(holds(t,a.normal)||holds(t,a.dialog)))&&!holds(numbers(d,w,a.state),a.skip);}
std::string errorText(int e){char b[AV_ERROR_MAX_STRING_SIZE]={};av_strerror(e,b,sizeof(b));return b;}
class Grabber final:public LinuxGrabber { public: Grabber(const std::string&id,int rate):display(connectToDisplay()),window(windowOf(id)),framerate(std::max(1,rate)),packet(av_packet_alloc()){if(!display||!grabDevice()){failureText="The X11 source could not be opened.";return;}if(window){if(showing(display,window)==Showing::Gone){failureText="The window no longer exists.";return;}if(has(display,&xcb_composite_id))xcb_composite_redirect_window(display,window,XCB_COMPOSITE_REDIRECT_AUTOMATIC);}else {auto ss=screenAreas(display);try{auto n=std::stoul(id.substr(7));if(!id.starts_with("screen:")||n>=ss.size())throw 0;screen=ss[n];}catch(...){failureText="The screen no longer exists.";}}} Result grab(AVFrame*into,std::chrono::milliseconds wait)override {if(!failureText.empty())return Result::Failed;if(closed)return Result::Closed;auto until=Clock::now()+wait;if(window){auto s=showing(display,window);auto a=inside(display,window);if(!display)return fail(AVERROR(EIO));if(s==Showing::Gone){closed=true;input.reset();return Result::Closed;}if(s==Showing::Hidden||a.empty()){input.reset();picture={};std::this_thread::sleep_until(std::min(until,Clock::now()+hiddenWait));return Result::Hidden;}auto now=Clock::now();if(!(a==windowArea)){windowArea=a;changed=now;}if(!(a==picture)&&(picture.empty()||now-changed>=resizeSettle)){picture=a;input.reset();}if(a.x>picture.x||a.y>picture.y||a.x+a.width<picture.x+picture.width||a.y+a.height<picture.y+picture.height){std::this_thread::sleep_until(until);return Result::NoFrame;}}if(!input){auto e=open();if(e<0){input.reset();if(window&&!failedBefore){failedBefore=true;return Result::NoFrame;}return fail(e);}}if(due>until){std::this_thread::sleep_until(until);return Result::NoFrame;}std::this_thread::sleep_until(due);auto now=Clock::now();due=(now-due>interval()?now:due)+interval();int e=av_read_frame(input.get(),packet.get());if(e<0){input.reset();if(window&&!failedBefore){failedBefore=true;return Result::NoFrame;}return fail(e);}failedBefore=false;auto*p=input->streams[0]->codecpar;av_frame_unref(into);into->format=p->format;into->width=p->width;into->height=p->height;into->pts=packet->pts;into->data[0]=packet->data;into->linesize[0]=p->width*4;into->buf[0]=std::exchange(packet->buf,nullptr);av_packet_unref(packet.get());return Result::Frame;} bool deliversOnlyChanges()const override{return false;}std::string name()const override{return"x11grab";}std::string failure()const override{return failureText;} private: std::chrono::microseconds interval()const{return std::chrono::microseconds(1000000/framerate);} int open(){auto a=window?picture:screen;AVDictionary*o=nullptr;av_dict_set_int(&o,"framerate",framerate,0);std::string sz=std::to_string(a.width)+"x"+std::to_string(a.height);av_dict_set(&o,"video_size",sz.c_str(),0);av_dict_set_int(&o,"grab_x",a.x,0);av_dict_set_int(&o,"grab_y",a.y,0);if(window)av_dict_set_int(&o,"window_id",window,0);AVFormatContext*r=nullptr;int e=avformat_open_input(&r,"",grabDevice(),&o);av_dict_free(&o);if(e<0)return e;input.reset(r);if(input->nb_streams!=1||input->streams[0]->codecpar->format!=AV_PIX_FMT_BGR0){input.reset();return AVERROR(ENOSYS);}due=Clock::now()+interval();return 0;} Result fail(int e){input.reset();failureText=std::string(window?"The window can no longer be captured (":"The screen can no longer be captured (")+errorText(e)+")";return Result::Failed;} Display display;xcb_window_t window;CaptureRect screen{};int framerate;std::unique_ptr<AVFormatContext,InputCloser> input;std::unique_ptr<AVPacket,PacketCloser> packet;Clock::time_point due{},changed{};CaptureRect windowArea{},picture{};bool failedBefore=false,closed=false;std::string failureText;};
}
bool usable(){static const bool answer=[] {auto d=connectToDisplay();return d&&grabDevice()&&!xwayland(d); }();return answer;}
CaptureSources screens(){CaptureSources r;if(!usable())return r;auto d=connectToDisplay();if(!d)return r;auto a=screenAreas(d);for(std::size_t i=0;i<a.size();++i)r.push_back({"screen:"+std::to_string(i),CaptureSource::Kind::Screen,"Screen "+std::to_string(i+1),a[i],0});return r;}
CaptureSources windows(){CaptureSources r;if(!usable())return r;auto d=connectToDisplay();if(!d)return r;Atoms a(d);for(auto w:topDown(d,a)){if(!shareable(d,a,w))continue;auto name=text(d,w,a.name);if(name.empty())name=text(d,w,XCB_ATOM_WM_NAME);if(name.empty())continue;auto p=numbers(d,w,a.pid);r.push_back({"window:"+std::to_string(w),CaptureSource::Kind::Window,std::move(name),insideOnRoot(d,w),p.empty()?0:p[0]});}return r;}
FrameSize sizeOf(const CaptureSource&s){if(s.kind==CaptureSource::Kind::Screen)return{s.geometry.width,s.geometry.height};auto d=connectToDisplay();auto w=windowOf(s.id);auto a=(d&&w&&showing(d,w)==Showing::Shown)?inside(d,w):CaptureRect{};return{a.width,a.height};}
void restoreIfMinimised(const CaptureSource&s){auto d=connectToDisplay();auto w=windowOf(s.id);if(!d||!w)return;auto a=numbers(d,w,atom(d,"WM_STATE"));if(!a.empty()&&a[0]==iconicState){Reply<xcb_generic_error_t>(xcb_request_check(d,xcb_map_window_checked(d,w)));xcb_flush(d);}}
std::unique_ptr<LinuxGrabber> open(const std::string&id,int rate){return std::make_unique<Grabber>(id,rate);}
#else
bool usable(){return false;} CaptureSources screens(){return{};} CaptureSources windows(){return{};} FrameSize sizeOf(const CaptureSource&){return{};} void restoreIfMinimised(const CaptureSource&){} std::unique_ptr<LinuxGrabber> open(const std::string&,int){return nullptr;}
#endif
} // namespace Styx::Core::Video::X11Capture
