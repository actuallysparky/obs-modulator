// SPDX-FileCopyrightText: 2026 Sparky and OBS Modulator contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "platform.hpp"
#include <obs.h>
#include <util/platform.h>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

static int checks;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);std::exit(1); } } while(0)
static std::mutex queue_mutex;
static std::deque<std::pair<obs_task_t,void *>> tasks;
static std::atomic<int> updates{0};
static std::atomic<size_t> peak_queue{0};
static std::atomic<int> pixel{255};
static std::atomic<int> rendered{0},delivered{0};
static uint64_t rss() { return test_platform::resident_bytes(); }
static double cpu() { return test_platform::cpu_seconds(); }
// Standalone SDK tests need an explicit effects path. OBS 32 retains this API
// without a replacement; keep deprecation suppression local to this fixture.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
static void add_test_data(const char *path) { if(path[0])obs_add_data_path(path); }
#pragma GCC diagnostic pop
static void ui_task(obs_task_t task,void *param,bool wait)
{
  if(wait){task(param);return;}
  std::lock_guard<std::mutex> lock(queue_mutex);tasks.emplace_back(task,param);peak_queue=std::max(peak_queue.load(),tasks.size());
}
static void drain()
{
  std::deque<std::pair<obs_task_t,void *>> q;
  {std::lock_guard<std::mutex> lock(queue_mutex);q.swap(tasks);}
  for(auto &t:q)t.first(t.second);
}
static void run(double seconds)
{
  auto end=std::chrono::steady_clock::now()+std::chrono::duration<double>(seconds);
  while(std::chrono::steady_clock::now()<end){drain();test_platform::process_events();std::this_thread::sleep_for(std::chrono::milliseconds(1));}
  drain();
}
// Frame-paced effects can take longer than a short sleep on software renderers.
// Preserve each state assertion, but wait for its observable result with a deadline.
template<typename Predicate> static bool wait_for(Predicate predicate)
{
  auto end=std::chrono::steady_clock::now()+std::chrono::seconds(5);
  while(!predicate() && std::chrono::steady_clock::now()<end)run(.01);
  return predicate();
}
#define WAIT(x) CHECK(wait_for([&](){return (x);}))
static void next_frames()
{
  const int before=delivered.load(); WAIT(delivered.load()>=before+2);
}
// The isolated fixture has only the controller's UI pump after libobs startup.
// Step exactly one queued pump to assert restart phase without wall-clock races.
static void next_controller_pump()
{
  auto end=std::chrono::steady_clock::now()+std::chrono::seconds(5);
  std::pair<obs_task_t,void *> task{nullptr,nullptr};
  while(std::chrono::steady_clock::now()<end){
    {std::lock_guard<std::mutex> lock(queue_mutex);
      if(!tasks.empty()){task=tasks.front();tasks.pop_front();}}
    if(task.first)break;
    test_platform::process_events();std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  CHECK(task.first);task.first(task.second);
}
static const char *source_name(void *){return "Modulator test source";}
static void *source_create(obs_data_t *,obs_source_t *s){return s;}
static void source_destroy(void *){}
static std::atomic<bool> integer_mode{false}, hide_gain{false};
static obs_properties_t *source_properties(void *)
{
  auto *p=obs_properties_create();auto *group=obs_properties_create();
  if(integer_mode)obs_properties_add_int(group,"gain","Gain",0,10,1);
  else obs_properties_add_float_slider(group,"gain","Gain",0,1,.001);
  obs_property_set_visible(obs_properties_get(group,"gain"),!hide_gain);
  obs_properties_add_float(group,"other","Other",0,1,.001);
  for(int i=0;i<32;++i){char key[16];snprintf(key,sizeof(key),"p%02d",i);obs_properties_add_float(group,key,key,0,1,.001);}
  obs_properties_add_group(p,"group","Controls",OBS_GROUP_NORMAL,group);return p;
}
static void source_update(void *,obs_data_t *){++updates;}
static uint32_t width(void *){return 1920;}
static uint32_t height(void *){return 1080;}
static void source_render(void *,gs_effect_t *)
{
  ++rendered;
  auto *effect=obs_get_base_effect(OBS_EFFECT_SOLID);auto *color=gs_effect_get_param_by_name(effect,"color");
  vec4 white;vec4_set(&white,1,1,1,1);gs_effect_set_vec4(color,&white);
  while(gs_effect_loop(effect,"Solid"))gs_draw_sprite(nullptr,0,1920,1080);
}
static double value(obs_source_t *s,const char *key="gain")
{auto *d=obs_source_get_settings(s);double v=obs_data_get_double(d,key);obs_data_release(d);return v;}
static void change(obs_source_t *s,const char *key,double v)
{auto *d=obs_data_create();obs_data_set_double(d,key,v);obs_source_update(s,d);obs_data_release(d);}
static obs_source_t *controller(obs_source_t *parent,const char *key="gain",const char *filter="")
{
  auto *d=obs_data_create();obs_data_set_string(d,"source_uuid","@parent");obs_data_set_string(d,"filter_uuid",filter);
  obs_data_set_string(d,"property",key);obs_data_set_int(d,"property_type",2);
  obs_data_set_double(d,"minimum",0);obs_data_set_double(d,"maximum",1);
  obs_data_set_double(d,"period",.4);obs_data_set_int(d,"waveform",2);
  auto *s=obs_source_create("sparky_lfo_modulator","test LFO",d,nullptr);obs_data_release(d);
  CHECK(s);obs_source_filter_add(parent,s);CHECK(!obs_source_enabled(s));return s;
}
static std::string status(obs_source_t *s)
{auto *p=obs_source_properties(s);std::string text=obs_property_description(obs_properties_get(p,"status"));obs_properties_destroy(p);return text;}
static void release_controller(obs_source_t *parent,obs_source_t *s)
{obs_source_filter_remove(parent,s);obs_source_release(s);run(.06);}
static void video_frame(void *,video_data *frame){++delivered;if(frame->data[0])pixel=frame->data[0][0];}

int main(int argc,char **argv){
  if(argc<6){fprintf(stderr,"Usage: test-plugin PLUGIN DATA TEST_SHADER GRAPHICS OBS_DATA [SHADERFILTER SHADER_DATA [SOAK_SECONDS REPORT_JSON]]\n");return 2;}
  test_platform::initialize();CHECK(obs_startup("en-US",nullptr,nullptr));obs_set_ui_task_handler(ui_task);
  // libobs queues its own UI-thread registration when the handler is installed.
  // Complete it before loading the plugin so it cannot inflate the pump backlog.
  drain();
  add_test_data(argv[5]);
  obs_video_info video={};video.graphics_module=argv[4];
  video.base_width=video.output_width=1920;video.base_height=video.output_height=1080;
  video.fps_num=60;video.fps_den=1;video.output_format=VIDEO_FORMAT_RGBA;
  CHECK(obs_reset_video(&video)==OBS_VIDEO_SUCCESS);
  obs_source_info info={};info.id="modulator_test_source";info.type=OBS_SOURCE_TYPE_INPUT;
  info.output_flags=OBS_SOURCE_VIDEO|OBS_SOURCE_CUSTOM_DRAW;info.get_name=source_name;info.create=source_create;info.destroy=source_destroy;
  info.get_properties=source_properties;info.update=source_update;info.get_width=width;info.get_height=height;info.video_render=source_render;
  obs_register_source(&info);
  obs_module_t *module=nullptr;CHECK(obs_open_module(&module,argv[1],argv[2])==MODULE_SUCCESS);CHECK(obs_init_module(module));
  auto *d=obs_data_create();obs_data_set_double(d,"gain",.37);obs_data_set_double(d,"other",.5);
  auto *source=obs_source_create("modulator_test_source","isolated test",d,nullptr);obs_data_release(d);CHECK(source);
  obs_set_output_source(0,source);video_scale_info scale={};scale.format=VIDEO_FORMAT_RGBA;scale.width=1920;scale.height=1080;
  obs_add_raw_video_callback(&scale,video_frame,nullptr);
  obs_source_inc_showing(source);run(.1);
  auto *lfo=controller(source);obs_source_set_enabled(lfo,true);run(.11);
  CHECK(obs_source_enabled(lfo));WAIT(status(lfo).find("Running")==0);WAIT(value(source)>0 && value(source)<1);
  change(source,"other",.8);run(.08);CHECK(obs_source_enabled(lfo));CHECK(value(source,"other")==.8);
  change(source,"gain",.937);run(.08);WAIT(!obs_source_enabled(lfo));CHECK(value(source)==.937);
  CHECK(status(lfo).find("Paused")==0);
  obs_source_set_enabled(lfo,true);next_controller_pump();CHECK(obs_source_enabled(lfo));CHECK(value(source)==0);
  obs_source_set_enabled(lfo,false);run(.05);double held=value(source);run(.1);next_frames();CHECK(value(source)==held);
  WAIT(status(lfo).find("Disabled")==0);
  obs_source_set_enabled(lfo,true);run(.07);obs_source_set_enabled(lfo,false);obs_source_set_enabled(lfo,true);next_controller_pump();CHECK(value(source)==0);
  obs_source_set_enabled(lfo,true);run(.08);obs_set_output_source(0,nullptr);obs_source_dec_showing(source);run(.08);
  held=value(source);run(.1);next_frames();CHECK(value(source)==held);CHECK(obs_source_enabled(lfo));
  obs_source_inc_showing(source);obs_set_output_source(0,source);next_controller_pump();CHECK(value(source)==0);
  auto *props=obs_source_properties(lfo);auto *saved=obs_source_get_settings(lfo);
  obs_properties_apply_settings(props,saved);CHECK(std::string(obs_data_get_string(saved,"property"))=="gain");
  CHECK(obs_data_get_double(saved,"maximum")==1);obs_data_release(saved);
  obs_property_button_clicked(obs_properties_get(props,"restart"),lfo);
  // Button return value means refresh, not success: restart deliberately returns false.
  obs_properties_destroy(props);
  next_controller_pump();CHECK(value(source)==0);
  props=obs_source_properties(lfo);obs_property_button_clicked(obs_properties_get(props,"restore"),lfo);obs_properties_destroy(props);
  run(.07);WAIT(!obs_source_enabled(lfo));WAIT(value(source)==held);
  obs_source_set_enabled(lfo,true);run(.07);
  auto *conflict=controller(source);obs_source_set_enabled(conflict,true);run(.07);WAIT(!obs_source_enabled(conflict));CHECK(obs_source_enabled(lfo));
  d=obs_source_get_settings(conflict);obs_data_set_string(d,"property","other");obs_source_update(conflict,d);obs_data_release(d);
  obs_source_set_enabled(conflict,true);run(.07);CHECK(obs_source_enabled(conflict));
  d=obs_source_get_settings(conflict);obs_data_set_string(d,"property","gain");obs_source_update(conflict,d);obs_data_release(d);
  run(.07);WAIT(!obs_source_enabled(conflict));CHECK(obs_source_enabled(lfo));
  release_controller(source,conflict);
  obs_source_set_name(source,"renamed source");run(.05);CHECK(obs_source_enabled(lfo));
  hide_gain=true;obs_source_update_properties(source);run(.08);WAIT(!obs_source_enabled(lfo));hide_gain=false;
  obs_source_set_enabled(lfo,true);run(.05);integer_mode=true;obs_source_update_properties(source);run(.08);WAIT(!obs_source_enabled(lfo));integer_mode=false;
  release_controller(source,lfo);
  // Native integer target, save/load and a rejected invalid period.
  integer_mode=true;lfo=controller(source);d=obs_source_get_settings(lfo);
  obs_data_set_int(d,"property_type",1);obs_data_set_double(d,"maximum",10);obs_source_update(lfo,d);obs_data_release(d);
  obs_source_set_enabled(lfo,true);run(.12);CHECK(obs_source_enabled(lfo));WAIT(value(source)==std::round(value(source)));
  auto *serialized=obs_save_source(lfo);CHECK(serialized);
  release_controller(source,lfo);lfo=obs_load_private_source(serialized);obs_data_release(serialized);CHECK(lfo);
  obs_source_filter_add(source,lfo);run(.08);CHECK(obs_source_enabled(lfo));
  d=obs_source_get_settings(lfo);CHECK(obs_data_get_int(d,"property_type")==1);CHECK(obs_data_get_double(d,"maximum")==10);obs_data_release(d);
  change(lfo,"period",0);run(.06);WAIT(!obs_source_enabled(lfo));release_controller(source,lfo);integer_mode=false;
  auto *remote=obs_source_create("modulator_test_source","remote target",nullptr,nullptr);CHECK(remote);
  lfo=controller(source);d=obs_source_get_settings(lfo);obs_data_set_string(d,"source_uuid",obs_source_get_uuid(remote));obs_source_update(lfo,d);obs_data_release(d);
  obs_source_set_enabled(lfo,true);run(.07);CHECK(obs_source_enabled(lfo));obs_source_remove(remote);run(.07);WAIT(!obs_source_enabled(lfo));
  props=obs_source_properties(lfo);saved=obs_source_get_settings(lfo);std::string missing=obs_data_get_string(saved,"source_uuid");
  obs_properties_apply_settings(props,saved);CHECK(std::string(obs_data_get_string(saved,"source_uuid"))==missing);
  obs_data_release(saved);obs_properties_destroy(props);obs_source_release(remote);release_controller(source,lfo);

  // Real Shaderfilter is loaded explicitly, never the user's scene collection.
  if(argc>=8 && argv[6][0]) {
  obs_module_t *shader_module=nullptr;
  CHECK(obs_open_module(&shader_module,argv[6],argv[7])==MODULE_SUCCESS);
  CHECK(obs_init_module(shader_module));
  d=obs_data_create();obs_data_set_bool(d,"from_file",true);obs_data_set_string(d,"shader_file_name",argv[3]);obs_data_set_double(d,"gain",.37);
  const int before_shader_frames=delivered.load();
  auto *shader=obs_source_create("shader_filter","test shader",d,nullptr);obs_data_release(d);CHECK(shader);obs_source_filter_add(source,shader);run(.3);
  props=obs_source_properties(shader);CHECK(obs_properties_get(props,"gain"));obs_properties_destroy(props);
  // Property metadata can exist before a software renderer has finished loading
  // the shader. Establish the real baseline pixels/settings before automation.
  WAIT(delivered.load()>before_shader_frames+2 && pixel.load()>0 && pixel.load()<200);
  CHECK(value(shader)==.37);
  const int baseline_pixel=pixel.load(), before_modulation_frames=delivered.load();
  lfo=controller(source,"gain",obs_source_get_uuid(shader));obs_source_set_enabled(lfo,true);run(.15);
  CHECK(obs_source_enabled(lfo));
  const bool shader_changed=wait_for([&](){return value(shader)!=.37;});
  std::fprintf(stderr,"Shaderfilter activation: enabled=%d showing=%d gain=%.17g frames=%d status=%s\n",
    obs_source_enabled(lfo),obs_source_showing(source),value(shader),delivered.load(),status(lfo).c_str());
  CHECK(shader_changed);CHECK(value(shader)>=0 && value(shader)<=1);
  WAIT(delivered.load()>0);CHECK(rendered.load()>0);
  WAIT(delivered.load()>before_modulation_frames && std::abs(pixel.load()-baseline_pixel)>=10 && pixel.load()<200);
  change(shader,"other",.2);run(.05);CHECK(obs_source_enabled(lfo));
  change(shader,"gain",.99);run(.07);WAIT(!obs_source_enabled(lfo));CHECK(value(shader)==.99);
  obs_source_set_enabled(lfo,true);run(.06);props=obs_source_properties(shader);
  auto *reload=obs_properties_get(props,"reload_effect");CHECK(reload);obs_property_button_clicked(reload,shader);obs_properties_destroy(props);
  run(.2);
  std::fprintf(stderr,"Shaderfilter reload: enabled=%d gain=%.17g status=%s\n",
    obs_source_enabled(lfo),value(shader),status(lfo).c_str());
  CHECK(obs_source_enabled(lfo));
  obs_source_filter_remove(source,shader);obs_source_release(shader);run(.07);WAIT(!obs_source_enabled(lfo));release_controller(source,lfo);

  } else {
    std::puts("Shaderfilter checks NOT RUN (use the separately configured shaderfilter test)");
  }
  CHECK(delivered.load()>0);CHECK(rendered.load()>0);
  double soak=argc>=9 ? std::strtod(argv[8],nullptr):0;
  std::vector<obs_source_t *> mappings;
  for(int i=0;i<32;++i){char key[16];snprintf(key,sizeof(key),"p%02d",i);auto *s=controller(source,key);mappings.push_back(s);}
  run(.5);int baseline_updates=updates;uint32_t baseline_frames=obs_get_total_frames(),baseline_lagged=obs_get_lagged_frames();
  const double baseline_cpu_start=cpu();
  run(soak>0 ? 10: .25);
  const double baseline_cpu=cpu()-baseline_cpu_start;
  const uint64_t baseline_render_ns=obs_get_average_frame_time_ns();
  uint32_t baseline_n=obs_get_total_frames()-baseline_frames,baseline_lag=obs_get_lagged_frames()-baseline_lagged;
  baseline_updates=updates-baseline_updates;
  for(auto *s:mappings)obs_source_set_enabled(s,true);
  run(.2);int before=updates;uint32_t frames=obs_get_total_frames(),lagged=obs_get_lagged_frames();
  const double active_cpu_start=cpu();uint64_t rss_start=rss(),rss_min=rss_start,rss_max=rss_start;
  const double duration=soak>0 ? soak:1;
  for(double elapsed=0;elapsed<duration;){double chunk=std::min(30.0,duration-elapsed);run(chunk);elapsed+=chunk;uint64_t bytes=rss();rss_min=std::min(bytes,rss_min);rss_max=std::max(bytes,rss_max);
    if(soak>0){printf("SOAK %.0f/%.0f seconds; frames=%u lagged=%u rss=%llu queue_peak=%zu\n",elapsed,duration,obs_get_total_frames()-frames,obs_get_lagged_frames()-lagged,static_cast<unsigned long long>(bytes),peak_queue.load());fflush(stdout);}}
  if(soak==0)WAIT(updates.load()>before);
  const double active_cpu=cpu()-active_cpu_start;
  const uint64_t active_render_ns=obs_get_average_frame_time_ns();
  int written=updates-before;uint32_t frame_n=obs_get_total_frames()-frames,lag_n=obs_get_lagged_frames()-lagged;
  for(auto *s:mappings)CHECK(obs_source_enabled(s));
  CHECK(written>0);CHECK(written<=static_cast<int>(frame_n)+5);
  CHECK(peak_queue<=1);
  for(auto *s:mappings)release_controller(source,s);
  obs_remove_raw_video_callback(video_frame,nullptr);obs_set_output_source(0,nullptr);
  obs_source_dec_showing(source);obs_source_release(source);run(.1);
  drain();
  if(argc>=10){FILE *f=fopen(argv[9],"w");CHECK(f);fprintf(f,"{\"checks\":%d,\"soak_seconds\":%.3f,\"mapping_count\":32,\"width\":1920,\"height\":1080,\"fps\":60,\"baseline_frames\":%u,\"baseline_lagged\":%u,\"baseline_updates\":%d,\"active_frames\":%u,\"active_lagged\":%u,\"batched_target_updates\":%d,\"baseline_cpu_seconds\":%.6f,\"active_cpu_seconds\":%.6f,\"baseline_render_ns\":%llu,\"active_render_ns\":%llu,\"rss_start\":%llu,\"rss_min\":%llu,\"rss_max\":%llu,\"rss_end\":%llu,\"queue_peak\":%zu}\n",checks,soak,baseline_n,baseline_lag,baseline_updates,frame_n,lag_n,written,baseline_cpu,active_cpu,static_cast<unsigned long long>(baseline_render_ns),static_cast<unsigned long long>(active_render_ns),static_cast<unsigned long long>(rss_start),static_cast<unsigned long long>(rss_min),static_cast<unsigned long long>(rss_max),static_cast<unsigned long long>(rss()),peak_queue.load());fclose(f);}
  obs_shutdown();std::printf("%d real-plugin checks passed\n",checks);return 0;
}
