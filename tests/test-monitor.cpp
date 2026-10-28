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
#include <map>
#include <thread>
#ifdef MONITOR_UI_TEST
#include "dock.hpp"
#include <QApplication>
#include <QMainWindow>
#include <QElapsedTimer>
#endif

namespace modulator { void monitor_test_pump(); void monitor_test_time(double); }
static int checks=0, writes=0, property_queries=0;
static bool hidden=false, integer=false;
static bool callback_fixture=false, callback_integer=false, callback_refresh=false;
static int callback_calls=0, unrelated_callbacks=0, callback_updates=0, callback_action=0;
static obs_source_t *callback_target=nullptr, *callback_controller=nullptr;
static std::map<void *,double> callback_positions;
#define CHECK(x) do {++checks;if(!(x)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
static const char *name(void *) { return "Monitor fixture"; }
static void *create(obs_data_t *,obs_source_t *s) { return s; }
static void destroy(void *) {}
static void update(void *data,obs_data_t *) { ++writes;if(data==callback_target)++callback_updates; }
static uint32_t size(void *) { return 16; }
static const auto main_thread=std::this_thread::get_id();
static void ui_task(obs_task_t task,void *param,bool wait) {
  if(wait || std::this_thread::get_id()==main_thread)task(param);
  // The isolated fixture advances the controller explicitly, not from ticks.
}
static bool numeric_edit(void *source,obs_properties_t *,obs_property_t *,obs_data_t *d) {
  ++callback_calls;
  const double position=callback_integer ? obs_data_get_int(d,"gesture"):obs_data_get_double(d,"gesture");
  if(callback_positions[source]==position)return false;
  callback_positions[source]=position;
  obs_data_set_double(d,"derived",position*3);
  if(callback_action==1)obs_source_update(static_cast<obs_source_t *>(source),d);
  if(callback_action==2)obs_data_set_double(d,"gesture",.333);
  if(callback_action==3 && callback_controller)obs_source_set_enabled(callback_controller,false);
  if(callback_action==4 && callback_controller)obs_source_remove(callback_controller);
  if(callback_action==5)obs_source_remove(static_cast<obs_source_t *>(source));
  return callback_refresh;
}
static bool unrelated_edit(obs_properties_t *,obs_property_t *,obs_data_t *) {++unrelated_callbacks;return false;}
static obs_properties_t *properties(void *data) {
  ++property_queries;
  auto *p=obs_properties_create();
  if(integer)obs_properties_add_int(p,"gain","Gain",0,10,1);
  else obs_properties_add_float(p,"gain","Gain",0,1,.001);
  obs_property_set_visible(obs_properties_get(p,"gain"),!hidden);
  obs_properties_add_float(p,"other","Other",0,1,.001);
  for(int i=0;i<32;++i){const std::string key="p"+std::to_string(i);obs_properties_add_float(p,key.c_str(),key.c_str(),0,1,.001);}
  if(callback_fixture) {
    if(data) {auto *settings=obs_source_get_settings(static_cast<obs_source_t *>(data));
      callback_positions[data]=callback_integer ? obs_data_get_int(settings,"gesture"):obs_data_get_double(settings,"gesture");
      obs_data_release(settings);}
    auto *group=obs_properties_create();
    auto *gesture=callback_integer ? obs_properties_add_int(group,"gesture","Gesture",0,10,1)
      :obs_properties_add_float(group,"gesture","Gesture",0,1,.001);
    obs_property_set_modified_callback2(gesture,numeric_edit,data);
    obs_properties_add_float(group,"derived","Derived value",0,30,.001);
    obs_properties_add_group(p,"callback_group","Native callbacks",OBS_GROUP_NORMAL,group);
    obs_property_set_modified_callback(obs_properties_get(p,"other"),unrelated_edit);
  }
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
  CHECK(feed->sample().empty());
  // Exact engine clock seam: the real controller, targets, watches and feed
  // execute unchanged. No wall-clock sleeps decide a finite boundary.
  using modulator::MonitorState;
  double shot_clock=1000;
  auto at=[&](double t){modulator::monitor_test_time(t);modulator::monitor_test_pump();};
  auto fresh=[&](int wave,bool ease_in,bool ease_out) {
    auto *c=controller(source);obs_source_set_enabled(c,false);
    auto *cfg=obs_source_get_settings(c);
    obs_data_set_bool(cfg,"one_shot",true);obs_data_set_bool(cfg,"ease_in",ease_in);obs_data_set_bool(cfg,"ease_out",ease_out);
    obs_data_set_int(cfg,"waveform",wave);obs_source_update(c,cfg);obs_data_release(cfg);return c;
  };
  auto remove=[&](obs_source_t *c){obs_source_filter_remove(source,c);obs_source_release(c);feed->clear();};
  for(int wave=0;wave<12;++wave)for(bool entry:{false,true})for(bool exit:{false,true}) {
    shot_clock+=20;change(source,"gain",.93);lfo=fresh(wave,entry,exit);
    const std::string shot_id=obs_source_get_uuid(lfo);CHECK(feed->play(shot_id));at(shot_clock);
    rows=feed->sample();CHECK(rows.size()==1 && rows[0].one_shot && rows[0].timed);
    CHECK(rows[0].phase_remaining==4 && rows[0].total_remaining==(exit ? 8:4));
    if(entry || wave==5)CHECK(rows[0].value==.93);
    CHECK(rows[0].state==(entry && wave!=5 ? MonitorState::EasingIn:MonitorState::Running));
    at(shot_clock+2);rows=feed->sample();CHECK(rows[0].phase_remaining==2);
    const double midpoint=rows[0].value;
    if(wave==0 && entry)CHECK(std::abs(midpoint-.865)<1e-12);
    at(shot_clock+4);rows=feed->sample();
    if(exit) {
      CHECK(rows[0].state==MonitorState::EasingOut && rows[0].total_remaining==4);
      at(shot_clock+8);rows=feed->sample();CHECK(rows[0].value==.93);
    } else if(wave!=5) {
      const bool high=wave==2 || wave==4 || (wave>=6 && wave<=8);
      CHECK(rows[0].value==(high ? .8:.2));
    }
    CHECK(!rows[0].enabled && rows[0].state==MonitorState::Completed);
    const double held=rows[0].value;const int completed_writes=writes;at(shot_clock+12);
    CHECK(feed->sample()[0].value==held && writes==completed_writes);
    remove(lfo);
  }
  // Retrigger during playback AND natural release preserves the first snapshot.
  shot_clock+=20;change(source,"gain",.93);lfo=fresh(2,true,true);
  const std::string shot_id=obs_source_get_uuid(lfo);CHECK(feed->play(shot_id));at(shot_clock);at(shot_clock+1);
  double current=feed->sample()[0].value;CHECK(feed->play(shot_id));at(shot_clock+1);CHECK(feed->sample()[0].value==current);
  at(shot_clock+5);CHECK(feed->sample()[0].state==MonitorState::EasingOut);
  at(shot_clock+6);current=feed->sample()[0].value;CHECK(feed->play(shot_id));at(shot_clock+6);
  CHECK(feed->sample()[0].value==current);at(shot_clock+14);CHECK(feed->sample()[0].value==.93);
  CHECK(feed->sample()[0].state==MonitorState::Completed);
  // Loading never replays a saved enabled one-shot.
  CHECK(feed->play(shot_id));at(shot_clock+15);auto *serialized=obs_save_source(lfo);remove(lfo);
  lfo=obs_load_private_source(serialized);obs_data_release(serialized);CHECK(lfo);obs_source_filter_add(source,lfo);
  at(shot_clock+16);CHECK(!obs_source_enabled(lfo));CHECK(feed->sample()[0].state!=MonitorState::Running);remove(lfo);
  // Outside edits beat a Play already pending, in every stage and at completion.
  for(double interrupt:{0.,1.,4.,5.,7.999}) {
    shot_clock+=20;change(source,"gain",.93);lfo=fresh(2,true,true);
    CHECK(feed->play(obs_source_get_uuid(lfo)));at(shot_clock);at(shot_clock+interrupt);
    CHECK(feed->play(obs_source_get_uuid(lfo)));change(source,"gain",.777);at(shot_clock+interrupt);
    rows=feed->sample();CHECK(rows[0].state==MonitorState::Paused && !rows[0].enabled && rows[0].value==.777);
    at(shot_clock+12);CHECK(feed->sample()[0].value==.777);remove(lfo);
  }
  // Hiding cancels, and showing cannot replay. Deliberately armed hidden runs wait.
  shot_clock+=20;change(source,"gain",.93);lfo=fresh(5,false,false);
  CHECK(feed->play(obs_source_get_uuid(lfo)));at(shot_clock);at(shot_clock+1);
  obs_source_dec_showing(source);at(shot_clock+2);CHECK(!obs_source_enabled(lfo));
  current=feed->sample()[0].value;obs_source_inc_showing(source);at(shot_clock+3);
  CHECK(feed->sample()[0].value==current && feed->sample()[0].state==MonitorState::Cancelled);
  obs_source_dec_showing(source);CHECK(feed->play(obs_source_get_uuid(lfo)));at(shot_clock+4);
  CHECK(feed->sample()[0].state==MonitorState::Waiting);obs_source_inc_showing(source);at(shot_clock+6);
  CHECK(feed->sample()[0].value==current && feed->sample()[0].phase_remaining==4);
  // A compatible shader/property refresh doesn't restart the shot.
  obs_source_update_properties(source);at(shot_clock+7);CHECK(feed->sample()[0].phase_remaining==3);
  // A configuration edit cancels rather than silently replaying a new trajectory.
  change(lfo,"period",2);at(shot_clock+8);CHECK(feed->sample()[0].state==MonitorState::Cancelled);remove(lfo);
  // Cancel hidden arming, change the target, then activate with the eye:
  // the old pending-Play readback must not reject the new deliberate activation.
  for(bool coalesced:{false,true}) {
    shot_clock+=200;change(source,"gain",.93);lfo=fresh(5,false,false);obs_source_dec_showing(source);
    CHECK(feed->play(obs_source_get_uuid(lfo)));at(shot_clock);
    CHECK(feed->sample()[0].state==MonitorState::Waiting);
    obs_source_set_enabled(lfo,false);if(!coalesced)at(shot_clock+1);
    change(source,"gain",.777);obs_source_set_enabled(lfo,true);obs_source_inc_showing(source);at(shot_clock+2);
    CHECK(obs_source_enabled(lfo) && feed->sample()[0].value==.777 && feed->sample()[0].state==MonitorState::Running);
    remove(lfo);
  }
  // A Play followed by eye-off before any pump must remain cancelled.
  shot_clock+=200;change(source,"gain",.93);lfo=fresh(2,false,false);
  CHECK(feed->play(obs_source_get_uuid(lfo)));obs_source_set_enabled(lfo,false);at(shot_clock);
  CHECK(!obs_source_enabled(lfo) && feed->sample()[0].value==.93);remove(lfo);
  // Huge frame gaps complete directly, retaining the final exact return value.
  shot_clock+=20;change(source,"gain",.93);lfo=fresh(11,false,true);
  CHECK(feed->play(obs_source_get_uuid(lfo)));at(shot_clock);at(shot_clock+100);
  CHECK(feed->sample()[0].state==MonitorState::Completed && feed->sample()[0].value==.93);remove(lfo);
  // A property-status signal runs after calculation but before batch commit.
  // Exercise genuine cancellation in that gap, rather than only before pump.
  struct Interruption { obs_source_t *target; obs_source_t *controller; int action; bool fired=false; };
  auto interrupt=[](void *p,calldata_t *) {
    auto &x=*static_cast<Interruption *>(p);if(x.fired)return;x.fired=true;
    if(x.action==0)change(x.target,"gain",.777);
    if(x.action==1)obs_source_set_enabled(x.controller,false);
    if(x.action==2){auto *props=obs_source_properties(x.controller);
      obs_property_button_clicked(obs_properties_get(props,"play"),x.controller);obs_properties_destroy(props);}
    if(x.action==4){hidden=true;obs_source_update_properties(x.target);}
    if(x.action==3){auto *props=obs_source_properties(x.controller);
      obs_property_button_clicked(obs_properties_get(props,"restore"),x.controller);obs_properties_destroy(props);}
  };
  for(int action=0;action<5;++action) {
    shot_clock+=200;change(source,"gain",.93);lfo=fresh(2,false,false);
    Interruption x{source,lfo,action};
    signal_handler_connect(obs_source_get_signal_handler(lfo),"update_properties",interrupt,&x);
    CHECK(feed->play(obs_source_get_uuid(lfo)));at(shot_clock);CHECK(x.fired);
    signal_handler_disconnect(obs_source_get_signal_handler(lfo),"update_properties",interrupt,&x);
    auto *readback=obs_source_get_settings(source);
    CHECK(obs_data_get_double(readback,"gain")== (action==0 ? .777:.93));obs_data_release(readback);
    if(action==4)CHECK(!feed->sample()[0].available);
    at(shot_clock+1);
    if(action==0)CHECK(feed->sample()[0].state==MonitorState::Paused);
    if(action==1)CHECK(!obs_source_enabled(lfo) && feed->sample()[0].value==.93);
    if(action==2)CHECK(obs_source_enabled(lfo));
    if(action==3)CHECK(!obs_source_enabled(lfo) && feed->sample()[0].value==.93);
    if(action==4)CHECK(!obs_source_enabled(lfo) && feed->sample()[0].state==MonitorState::Error);
    remove(lfo);hidden=false;obs_source_update_properties(source);
  }
  // Even a fresh Play captures enough readback to yield to a later operator edit.
  shot_clock+=200;change(source,"gain",.93);lfo=fresh(2,false,false);
  CHECK(feed->play(obs_source_get_uuid(lfo)));change(source,"gain",.777);at(shot_clock);
  CHECK(feed->sample()[0].state==MonitorState::Paused && feed->sample()[0].value==.777);remove(lfo);
  // Explicit independent false survives migration and reload; old checked migrates both.
  auto *legacy=obs_data_create();obs_data_set_bool(legacy,"ease_in",true);
  lfo=obs_source_create("sparky_lfo_modulator","legacy checked",legacy,nullptr);obs_data_release(legacy);
  auto *settings=obs_source_get_settings(lfo);CHECK(obs_data_get_bool(settings,"ease_out"));
  obs_data_set_bool(settings,"ease_out",false);obs_source_update(lfo,settings);obs_data_release(settings);
  serialized=obs_save_source(lfo);obs_source_release(lfo);lfo=obs_load_private_source(serialized);obs_data_release(serialized);
  settings=obs_source_get_settings(lfo);CHECK(obs_data_get_bool(settings,"ease_in") && !obs_data_get_bool(settings,"ease_out"));
  obs_data_release(settings);obs_source_release(lfo);
  // Generic native property edits, without any Shaderfilter-specific behavior.
  // A non-video remote target makes update ordering and batching synchronous.
  callback_fixture=true;info.id="callback_fixture";info.output_flags=0;obs_register_source(&info);
  callback_target=obs_source_create("callback_fixture","Callback target",nullptr,nullptr);CHECK(callback_target);
  auto target_value=[&](const char *key){auto *v=obs_source_get_settings(callback_target);
    const double result=obs_data_get_double(v,key);obs_data_release(v);return result;};
  auto callback_shot=[&](bool is_integer=false) {
    callback_integer=is_integer;shot_clock+=200;
    lfo=controller(source,"gesture",false,is_integer ? 1:2);
    auto *cfg=obs_source_get_settings(lfo);obs_data_set_string(cfg,"source_uuid",obs_source_get_uuid(callback_target));
    obs_data_set_double(cfg,"minimum",0);obs_data_set_double(cfg,"maximum",is_integer ? 10:1);
    obs_data_set_double(cfg,"period",4);obs_data_set_int(cfg,"waveform",2);obs_data_set_bool(cfg,"one_shot",true);
    obs_source_update(lfo,cfg);obs_data_release(cfg);callback_controller=lfo;
    CHECK(feed->play(obs_source_get_uuid(lfo)));at(shot_clock);
  };
  callback_shot();CHECK(target_value("gesture")==0 && target_value("derived")==0);
  const int calls_before=callback_calls, unrelated_before=unrelated_callbacks, queries_before=property_queries;
  const int updates_before=callback_updates;
  at(shot_clock+.5);CHECK(target_value("gesture")==.125 && target_value("derived")==.375);
  CHECK(callback_calls==calls_before+1 && unrelated_callbacks==unrelated_before);
  CHECK(property_queries==queries_before && callback_updates==updates_before+1);
  at(shot_clock+4);CHECK(target_value("gesture")==1 && target_value("derived")==3);
  CHECK(!obs_source_enabled(lfo) && feed->sample()[0].state==MonitorState::Completed);
  at(shot_clock+8);CHECK(target_value("derived")==3);
  // Restore goes through the same callback path, not only the main pump.
  auto *callback_props=obs_source_properties(lfo);
  obs_property_button_clicked(obs_properties_get(callback_props,"restore"),lfo);obs_properties_destroy(callback_props);
  at(shot_clock+9);CHECK(target_value("gesture")==0 && target_value("derived")==0);remove(lfo);
  callback_shot();auto *second=controller(source,"other",false,2);
  auto *second_cfg=obs_source_get_settings(second);
  obs_data_set_string(second_cfg,"source_uuid",obs_source_get_uuid(callback_target));
  obs_data_set_double(second_cfg,"minimum",0);obs_data_set_double(second_cfg,"maximum",1);
  obs_data_set_double(second_cfg,"period",4);obs_data_set_int(second_cfg,"waveform",2);obs_data_set_bool(second_cfg,"one_shot",true);
  obs_source_update(second,second_cfg);obs_data_release(second_cfg);
  CHECK(feed->play(obs_source_get_uuid(second)));at(shot_clock);
  const int batch_before=callback_updates, batch_callbacks=callback_calls, batch_other=unrelated_callbacks;
  at(shot_clock+.5);CHECK(callback_updates==batch_before+1);
  CHECK(callback_calls==batch_callbacks+1 && unrelated_callbacks==batch_other+1);
  CHECK(target_value("gesture")==.125 && target_value("other")==.125 && target_value("derived")==.375);
  at(shot_clock+4);CHECK(!obs_source_enabled(lfo) && !obs_source_enabled(second));remove(lfo);remove(second);
  callback_shot(true);at(shot_clock+2);CHECK(target_value("gesture")==5 && target_value("derived")==15);
  at(shot_clock+4);CHECK(target_value("gesture")==10 && target_value("derived")==30);remove(lfo);
  callback_shot();callback_action=1;at(shot_clock+.5);
  CHECK(obs_source_enabled(lfo) && target_value("derived")==.375);callback_action=0;
  change(callback_target,"gesture",.777);at(shot_clock+1);
  CHECK(!obs_source_enabled(lfo) && target_value("gesture")==.777);remove(lfo);
  // Operator takeover includes numeric values changed by the native callback.
  callback_shot();at(shot_clock+.5);change(callback_target,"derived",.99);at(shot_clock+1);
  CHECK(!obs_source_enabled(lfo) && target_value("gesture")==.125 && target_value("derived")==.99);remove(lfo);
  // A discovered dependent setting is owned along with its compound control.
  callback_shot();at(shot_clock+.5);second=controller(source,"derived",false,2);
  second_cfg=obs_source_get_settings(second);obs_data_set_string(second_cfg,"source_uuid",obs_source_get_uuid(callback_target));
  obs_source_update(second,second_cfg);obs_data_release(second_cfg);obs_source_set_enabled(second,true);at(shot_clock+.75);
  CHECK(obs_source_enabled(lfo) && !obs_source_enabled(second));remove(second);remove(lfo);
  // Discovering a NEW dependency cannot steal a field from an earlier owner,
  // even when the newcomer submits a synchronous update inside its callback.
  second=controller(source,"derived",false,2);second_cfg=obs_source_get_settings(second);
  obs_data_set_string(second_cfg,"source_uuid",obs_source_get_uuid(callback_target));
  obs_data_set_int(second_cfg,"waveform",2);obs_data_set_double(second_cfg,"minimum",0);obs_data_set_double(second_cfg,"maximum",1);
  obs_source_update(second,second_cfg);obs_data_release(second_cfg);obs_source_set_enabled(second,true);
  shot_clock+=200;at(shot_clock);callback_shot();callback_action=1;at(shot_clock+.5);callback_action=0;
  CHECK(!obs_source_enabled(lfo) && obs_source_enabled(second) && target_value("derived")==.125);
  at(shot_clock+.75);CHECK(obs_source_enabled(second) && target_value("derived")==.1875);remove(lfo);remove(second);
  callback_shot();callback_refresh=true;at(shot_clock+.5);callback_refresh=false;
  at(shot_clock+1);CHECK(obs_source_enabled(lfo) && target_value("derived")==.75);remove(lfo);
  callback_shot();callback_action=2;at(shot_clock+.5);callback_action=0;
  CHECK(!obs_source_enabled(lfo) && target_value("gesture")==.333);remove(lfo);
  callback_shot();callback_action=3;at(shot_clock+.5);callback_action=0;
  CHECK(!obs_source_enabled(lfo));at(shot_clock+1);CHECK(target_value("gesture")==.125);remove(lfo);
  callback_shot();callback_action=4;at(shot_clock+.5);callback_action=0;
  CHECK(obs_source_removed(lfo));remove(lfo);
  callback_shot();callback_action=5;at(shot_clock+.5);callback_action=0;
  CHECK(obs_source_removed(callback_target) && !obs_source_enabled(lfo));remove(lfo);
  callback_controller=nullptr;obs_source_release(callback_target);callback_target=nullptr;callback_fixture=false;
  modulator::monitor_test_time(-1);feed.reset();
  obs_source_dec_showing(source);obs_source_release(source);obs_module_unload();obs_shutdown();
  std::printf("%d monitor integration checks passed\n",checks);
}
