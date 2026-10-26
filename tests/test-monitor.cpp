// SPDX-FileCopyrightText: 2026 Sparky and OBS Modulator contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "monitor.hpp"
#include "platform.hpp"
#include <obs-module.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>
#ifdef MONITOR_UI_TEST
#include "dock.hpp"
#include <QApplication>
#include <QMainWindow>
#include <QElapsedTimer>
#endif

namespace modulator { void monitor_test_pump(); }
static int checks=0, writes=0, property_queries=0;
static bool hidden=false, integer=false;
#define CHECK(x) do {++checks;if(!(x)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
static const char *name(void *) { return "Monitor fixture"; }
static void *create(obs_data_t *,obs_source_t *s) { return s; }
static void destroy(void *) {}
static void update(void *,obs_data_t *) { ++writes; }
static uint32_t size(void *) { return 16; }
static const auto main_thread=std::this_thread::get_id();
static void ui_task(obs_task_t task,void *param,bool wait) {
  if(wait || std::this_thread::get_id()==main_thread)task(param);
  // The isolated fixture advances the controller explicitly, not from ticks.
}
static obs_properties_t *properties(void *) {
  ++property_queries;
  auto *p=obs_properties_create();
  if(integer)obs_properties_add_int(p,"gain","Gain",0,10,1);
  else obs_properties_add_float(p,"gain","Gain",0,1,.001);
  obs_property_set_visible(obs_properties_get(p,"gain"),!hidden);
  obs_properties_add_float(p,"other","Other",0,1,.001);
  for(int i=0;i<32;++i){const std::string key="p"+std::to_string(i);obs_properties_add_float(p,key.c_str(),key.c_str(),0,1,.001);}
  return p;
}
static void change(obs_source_t *s,const char *key,double value) {
  auto *d=obs_data_create();obs_data_set_double(d,key,value);obs_source_update(s,d);obs_data_release(d);
}
static obs_source_t *controller(obs_source_t *parent,const char *key="gain",bool ease=false,int type=2) {
  auto *d=obs_data_create();obs_data_set_bool(d,"configured",true);obs_data_set_string(d,"source_uuid","@parent");
  obs_data_set_string(d,"property",key);obs_data_set_int(d,"property_type",type);
  obs_data_set_double(d,"minimum",.2);obs_data_set_double(d,"maximum",.8);obs_data_set_double(d,"period",4);
  obs_data_set_bool(d,"ease_in",ease);obs_data_set_int(d,"waveform",5);
  auto *s=obs_source_create("sparky_lfo_modulator","LFO fixture",d,nullptr);obs_data_release(d);
  obs_source_filter_add(parent,s);return s;
}
int main(int argc,char **argv) {
#ifdef MONITOR_UI_TEST
  QApplication app(argc,argv);
#endif
  CHECK(argc==3);test_platform::initialize();
  CHECK(obs_startup("en-US",nullptr,nullptr));
  obs_set_ui_task_handler(ui_task);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  obs_add_data_path(argv[2]);
#pragma GCC diagnostic pop
  obs_video_info video={};video.graphics_module=argv[1];video.base_width=video.output_width=16;
  video.base_height=video.output_height=16;video.fps_num=30;video.fps_den=1;video.output_format=VIDEO_FORMAT_RGBA;
  CHECK(obs_reset_video(&video)==OBS_VIDEO_SUCCESS);
  obs_source_info info={};info.id="monitor_fixture";info.type=OBS_SOURCE_TYPE_INPUT;info.output_flags=OBS_SOURCE_VIDEO;
  info.get_name=name;info.create=create;info.destroy=destroy;info.update=update;info.get_properties=properties;
  info.get_width=size;info.get_height=size;obs_register_source(&info);
  CHECK(obs_module_load());
  auto *d=obs_data_create();obs_data_set_double(d,"gain",.93);obs_data_set_double(d,"other",.6);
  auto *source=obs_source_create("monitor_fixture","Parent",d,nullptr);obs_data_release(d);CHECK(source);
  auto *lfo=controller(source,"gain",true);obs_source_set_enabled(lfo,false);
  auto feed=modulator::make_monitor_feed();auto rows=feed->sample();CHECK(rows.size()==1);
  const auto id=rows[0].id;CHECK(rows[0].available && rows[0].value==.93 && !rows[0].enabled);
  CHECK(rows[0].low==.2 && rows[0].high==.8 && rows[0].property=="Gain");
  const int before=writes, queries=property_queries;
  for(int i=0;i<50;++i){rows=feed->sample();CHECK(rows[0].value==.93);}
  CHECK(writes==before);CHECK(property_queries==queries);
  CHECK(feed->set_enabled(id,true));modulator::monitor_test_pump();
  rows=feed->sample();CHECK(rows[0].state==modulator::MonitorState::Waiting);
  obs_source_inc_showing(source);modulator::monitor_test_pump();rows=feed->sample();
  CHECK(rows[0].state==modulator::MonitorState::EasingIn);CHECK(rows[0].value==.93);
  CHECK(feed->set_enabled(id,false));modulator::monitor_test_pump();rows=feed->sample();
  CHECK(!rows[0].enabled && rows[0].state==modulator::MonitorState::EasingOut);
  CHECK(feed->set_enabled(id,true));modulator::monitor_test_pump();
  change(source,"gain",.777);modulator::monitor_test_pump();rows=feed->sample();
  CHECK(!rows[0].enabled && rows[0].state==modulator::MonitorState::Paused && rows[0].value==.777);
  obs_source_set_name(source,"Renamed parent");obs_source_set_name(lfo,"Renamed LFO");rows=feed->sample();
  CHECK(rows[0].source=="Renamed parent" && rows[0].name=="Renamed LFO" && rows[0].id==id);
  hidden=true;obs_source_update_properties(source);rows=feed->sample();CHECK(!rows[0].available);
  hidden=false;obs_source_update_properties(source);rows=feed->sample();CHECK(rows[0].available);
  auto *cfg=obs_data_create();obs_data_set_string(cfg,"property","other");obs_source_update(lfo,cfg);obs_data_release(cfg);
  rows=feed->sample();CHECK(rows[0].value==.6 && rows[0].property=="Other");
  feed->clear();CHECK(feed->sample()[0].value==.6);
  obs_source_filter_remove(source,lfo);obs_source_release(lfo);CHECK(feed->sample().empty());CHECK(!feed->set_enabled(id,true));
  integer=true;obs_source_update_properties(source);
  auto *intdata=obs_data_create();obs_data_set_int(intdata,"gain",4);obs_source_update(source,intdata);obs_data_release(intdata);
  lfo=controller(source,"gain",false,1);obs_source_set_enabled(lfo,false);rows=feed->sample();
  CHECK(rows[0].integer && rows[0].value==4);
  obs_source_filter_remove(source,lfo);obs_source_release(lfo);integer=false;
  std::vector<obs_source_t *> population;
  for(int i=0;i<32;++i){const std::string key="p"+std::to_string(i);auto *c=controller(source,key.c_str());obs_source_set_enabled(c,false);population.push_back(c);}
  rows=feed->sample();CHECK(rows.size()==32);
  std::vector<double> elapsed;
  for(int i=0;i<200;++i) {
    const auto start=std::chrono::steady_clock::now();rows=feed->sample();
    elapsed.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());
    CHECK(rows.size()==32);
  }
  std::sort(elapsed.begin(),elapsed.end());
  std::printf("32-controller readback snapshot median/p95 %.3f/%.3f ms\n",elapsed[100],elapsed[190]);
#ifdef MONITOR_UI_TEST
  // Measure the real backend and normal incremental UI painting together,
  // rather than adding unrelated timings or repeatedly repainting the shell.
  QMainWindow main;main.resize(510,360);
  auto *panel=new modulator::MonitorPanel(modulator::make_monitor_feed(),&main);main.setCentralWidget(panel);
  main.show();QCoreApplication::processEvents();panel->set_exposed(true);
  for(auto *c:population)obs_source_set_enabled(c,true);
  modulator::monitor_test_pump();panel->test_refresh();QCoreApplication::processEvents();
  std::vector<double> live;
  for(int i=0;i<200;++i) {
    modulator::monitor_test_pump();QElapsedTimer t;t.start();panel->test_refresh();
    QCoreApplication::processEvents();QCoreApplication::sendPostedEvents(nullptr,QEvent::UpdateRequest);
    live.push_back(t.nsecsElapsed()/1e6);
  }
  std::sort(live.begin(),live.end());CHECK(panel->test_paints()>0);
  std::printf("32-controller REAL snapshot/model/incremental-paint median/p95 %.3f/%.3f ms\n",live[100],live[190]);
  panel->set_exposed(false);QCoreApplication::processEvents();
  const auto hidden_samples=panel->samples,hidden_ticks=panel->timer_callbacks,hidden_paints=panel->test_paints();
  for(int i=0;i<200;++i){modulator::monitor_test_pump();QCoreApplication::processEvents();}
  CHECK(panel->samples==hidden_samples && panel->timer_callbacks==hidden_ticks && panel->test_paints()==hidden_paints);
  main.hide();main.takeCentralWidget();delete panel;
#endif
  for(auto *c:population){obs_source_filter_remove(source,c);obs_source_release(c);}
  CHECK(feed->sample().empty());feed.reset();
  obs_source_dec_showing(source);obs_source_release(source);obs_module_unload();obs_shutdown();
  std::printf("%d monitor integration checks passed\n",checks);
}
