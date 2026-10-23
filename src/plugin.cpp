// SPDX-FileCopyrightText: 2026 Sparky and OBS Modulator contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include <obs-module.h>
#include <util/platform.h>
#include "lfo.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-modulator", "en-US")
MODULE_EXPORT const char *obs_module_description(void)
{ return "LFO automation of numeric source and filter settings"; }
MODULE_EXPORT const char *obs_module_author(void) { return "Sparky"; }

namespace {
constexpr const char *ID = "sparky_lfo_modulator";
using Source = std::unique_ptr<obs_source_t, decltype(&obs_source_release)>;
using Data = std::unique_ptr<obs_data_t, decltype(&obs_data_release)>;
using Props = std::unique_ptr<obs_properties_t, decltype(&obs_properties_destroy)>;
Source own(obs_source_t *p) { return Source(p, obs_source_release); }
Data settings(obs_source_t *p) { return Data(obs_source_get_settings(p), obs_data_release); }
Props properties(obs_source_t *p) { return Props(obs_source_properties(p), obs_properties_destroy); }
bool is_controller(obs_source_t *p) { return std::string(obs_source_get_unversioned_id(p)) == ID; }

struct Numeric {
  std::string key, label;
  bool integer = false;
  double low = 0, high = 0, step = 0.01;
};
void numeric_properties(obs_properties_t *props, std::vector<Numeric> &out, const std::string &prefix = "")
{
  for (auto *p = obs_properties_first(props); p; obs_property_next(&p)) {
    if (!obs_property_visible(p) || !obs_property_enabled(p)) continue;
    const auto type = obs_property_get_type(p);
    const std::string label = prefix + obs_property_description(p);
    if (type == OBS_PROPERTY_GROUP) {
      numeric_properties(obs_property_group_content(p), out, label + " / ");
    } else if (type == OBS_PROPERTY_INT || type == OBS_PROPERTY_FLOAT) {
      Numeric n; n.key = obs_property_name(p); n.label = label; n.integer = type == OBS_PROPERTY_INT;
      n.low = n.integer ? obs_property_int_min(p) : obs_property_float_min(p);
      n.high = n.integer ? obs_property_int_max(p) : obs_property_float_max(p);
      n.step = n.integer ? obs_property_int_step(p) : obs_property_float_step(p);
      if (std::isfinite(n.low) && std::isfinite(n.high) && n.low <= n.high) out.push_back(n);
    }
  }
}
double read_value(obs_source_t *target, const Numeric &n)
{
  auto d = settings(target);
  return n.integer ? static_cast<double>(obs_data_get_int(d.get(), n.key.c_str()))
                   : obs_data_get_double(d.get(), n.key.c_str());
}

struct Watch {
  obs_weak_source_t *target;
  Numeric number;
  std::atomic<double> expected{0};
  std::atomic<uint64_t> write_generation{0};
  std::atomic<bool> wrote{false}, external{false}, dirty{false};
  static void changed(void *param, calldata_t *)
  {
    auto *w = static_cast<Watch *>(param);
    const uint64_t before=w->write_generation.load();
    if(before & 1) return;
    auto target = own(obs_weak_source_get_source(w->target));
    if(!target || !w->wrote)return;
    const double current=read_value(target.get(),w->number), expected=w->expected.load();
    if(before!=w->write_generation.load())return;
    if(!modulator::same_value(current,expected,w->number.integer))w->external=true;
  }
  static void schema(void *param, calldata_t *) { static_cast<Watch *>(param)->dirty = true; }
  Watch(obs_source_t *source, Numeric n) : target(obs_source_get_weak_source(source)), number(std::move(n))
  {
    auto *signals = obs_source_get_signal_handler(source);
    signal_handler_connect(signals, "update", changed, this);
    signal_handler_connect(signals, "update_properties", schema, this);
  }
  ~Watch()
  {
    auto source = own(obs_weak_source_get_source(target));
    if (source) {
      auto *signals = obs_source_get_signal_handler(source.get());
      signal_handler_disconnect(signals, "update", changed, this);
      signal_handler_disconnect(signals, "update_properties", schema, this);
    }
    obs_weak_source_release(target);
  }
};

struct Config {
  std::string source, filter, key;
  int type = 0, wave = 0;
  double low = 0, high = 0, period = 4;
  bool ease_in = false;
  bool operator==(const Config &b) const {
    return source==b.source && filter==b.filter && key==b.key && type==b.type &&
      wave==b.wave && low==b.low && high==b.high && period==b.period && ease_in==b.ease_in;
  }
};
Config config(obs_data_t *d)
{
  return {obs_data_get_string(d,"source_uuid"), obs_data_get_string(d,"filter_uuid"),
    obs_data_get_string(d,"property"), static_cast<int>(obs_data_get_int(d,"property_type")),
    static_cast<int>(obs_data_get_int(d,"waveform")), obs_data_get_double(d,"minimum"),
    obs_data_get_double(d,"maximum"), obs_data_get_double(d,"period"), obs_data_get_bool(d,"ease_in")};
}
struct Controller {
  obs_weak_source_t *self;
  std::recursive_mutex mutex;
  std::atomic<bool> dead{false}, restart{false}, restore{false};
  std::atomic<bool> gate_changed{false};
  std::atomic<bool> initialized{false};
  Config last;
  std::unique_ptr<Watch> watch;
  bool active = false, snapshot_valid = false;
  double started = 0, checked = 0, snapshot = 0, ease_start = 0;
  uint64_t glider_seed = UINT64_C(14695981039346656037);
  std::string snapshot_uuid, snapshot_key, status = "Disabled — choose a target, then enable the eye";
  std::string identity, ui_source, ui_filter, ui_key;
  bool snapshot_integer = false;
  static void enabled(void *p, calldata_t *) { static_cast<Controller *>(p)->gate_changed=true; }
  explicit Controller(obs_source_t *s) : self(obs_source_get_weak_source(s)), identity(obs_source_get_uuid(s))
  {
    // The saved filter UUID gives each mapping independent, reproducible motion.
    for (unsigned char ch : identity) { glider_seed ^= ch; glider_seed *= UINT64_C(1099511628211); }
    signal_handler_connect(obs_source_get_signal_handler(s),"enable",enabled,this);
  }
  ~Controller() {
    auto s=own(obs_weak_source_get_source(self));
    if(s)signal_handler_disconnect(obs_source_get_signal_handler(s.get()),"enable",enabled,this);
    watch.reset(); obs_weak_source_release(self);
  }
};
using Handle = std::shared_ptr<Controller>;
std::mutex registry_mutex;
std::vector<Handle> controllers;
std::atomic<bool> pending{false}, stopping{false};

Source resolve(obs_source_t *self, const Config &c)
{
  Source parent = c.source == "@parent" ? own(obs_source_get_ref(obs_filter_get_parent(self)))
                                        : own(obs_get_source_by_uuid(c.source.c_str()));
  if (!parent || obs_source_removed(parent.get()) || is_controller(parent.get())) return own(nullptr);
  if (c.filter.empty()) return parent;
  struct Find { const std::string &uuid; obs_source_t *source = nullptr; } find{c.filter};
  obs_source_enum_filters(parent.get(), [](obs_source_t *, obs_source_t *f, void *p) {
    auto &v = *static_cast<Find *>(p);
    if (v.uuid == obs_source_get_uuid(f)) v.source = obs_source_get_ref(f);
  }, &find);
  auto found = own(find.source);
  if (found && (obs_source_removed(found.get()) || is_controller(found.get()))) return own(nullptr);
  return found;
}
bool find_numeric(obs_source_t *target, const std::string &key, Numeric &n)
{
  if (!target) return false;
  auto p = properties(target); std::vector<Numeric> all;
  numeric_properties(p.get(), all);
  auto it = std::find_if(all.begin(), all.end(), [&](const Numeric &v) {return v.key == key;});
  if (it == all.end()) return false;
  n = *it; return true;
}
void status(Controller &c, obs_source_t *self, const std::string &message)
{
  if (c.status == message) return;
  c.status = message;
  obs_source_update_properties(self);
}
void disable(Controller &c, obs_source_t *self, const char *why)
{
  c.active = false; c.watch.reset();
  obs_source_set_enabled(self, false);
  status(c, self, why);
}
void write(Data &delta, const Numeric &n, double value)
{
  if (n.integer) obs_data_set_int(delta.get(), n.key.c_str(), static_cast<long long>(value));
  else obs_data_set_double(delta.get(), n.key.c_str(), value);
}

struct Batch {
  Source target{nullptr, obs_source_release};
  Data delta{obs_data_create(), obs_data_release};
  struct Change { Handle controller; Numeric number; double observed, value; };
  std::vector<Change> changes;
  std::vector<Watch *> writing;
};
void pump(void *)
{
  // Only this UI task performs writes. One queued task globally, not one per filter/frame.
  if (stopping) { pending = false; return; }
  std::vector<Handle> list;
  { std::lock_guard<std::mutex> lock(registry_mutex); list = controllers; }
  // A mapping edited onto another owner's property is a newcomer, even if its
  // previous mapping was active. Do not let UUID ordering displace that owner.
  for(auto &h:list){std::lock_guard<std::recursive_mutex> lock(h->mutex);
    auto self=own(obs_weak_source_get_source(h->self));
    if(self){auto d=settings(self.get());if(!(config(d.get())==h->last))h->active=false;}}
  std::sort(list.begin(), list.end(), [](const Handle &a, const Handle &b) {
    // Established owners precede newcomers, including after copying filters.
    if (a->active != b->active) return a->active > b->active;
    return a->identity < b->identity;
  });
  std::map<std::string, Controller *> owners;
  std::map<std::string, Batch> batches;
  const double now = static_cast<double>(os_gettime_ns()) / 1e9;
  for (auto &h : list) {
    auto &c = *h; std::lock_guard<std::recursive_mutex> lock(c.mutex);
    if (c.dead) continue;
    auto self = own(obs_weak_source_get_source(c.self));
    if (!self || obs_source_removed(self.get())) continue;
    if(c.gate_changed.exchange(false)){c.active=false;c.watch.reset();}
    auto d = settings(self.get()); const Config cfg = config(d.get());
    if (c.restore.exchange(false)) {
      disable(c,self.get(),"Disabled — restored starting value");
      auto target = resolve(self.get(), cfg); Numeric n;
      if (c.snapshot_valid && target && c.snapshot_uuid == obs_source_get_uuid(target.get()) &&
          c.snapshot_key == cfg.key && find_numeric(target.get(),cfg.key,n) && n.integer==c.snapshot_integer) {
        Data delta(obs_data_create(),obs_data_release);
        write(delta,n,std::clamp(c.snapshot,n.low,n.high));
        obs_source_update(target.get(),delta.get());
      } else status(c,self.get(),"Disabled — starting value unavailable for this target");
      continue;
    }
    if (!(cfg == c.last)) { c.active=false; c.watch.reset(); c.last=cfg; }
    auto *parent = obs_filter_get_parent(self.get());
    if (!obs_source_enabled(self.get())) {
      c.active=false; c.watch.reset(); c.restart=false;
      if (c.status.rfind("Running",0)==0 || c.status.rfind("Waiting",0)==0)
        status(c,self.get(),"Disabled — holding the last value");
      continue;
    }
    if (!parent || !obs_source_showing(parent)) {
      c.active=false; c.watch.reset(); status(c,self.get(),"Waiting — parent source is hidden"); continue;
    }
    if (cfg.key.empty()) { disable(c,self.get(),"Disabled — choose a numeric target"); continue; }
    if (!std::isfinite(cfg.low) || !std::isfinite(cfg.high) || cfg.low>cfg.high ||
        !std::isfinite(cfg.period) || cfg.period<0.05 || cfg.period>86400 || cfg.wave<0 || cfg.wave>5) {
      disable(c,self.get(),"Disabled — invalid range, waveform, or cycle time"); continue;
    }
    auto target = resolve(self.get(),cfg);
    if (!target) { disable(c,self.get(),"Disabled — target was removed; select it again"); continue; }
    const std::string uuid = obs_source_get_uuid(target.get());
    const std::string ownership = uuid + "/" + cfg.key;
    if (owners.count(ownership)) { disable(c,self.get(),"Disabled — another LFO owns this property"); continue; }
    owners[ownership]=&c;
    if (!c.watch || now-c.checked>=1 || c.watch->dirty.exchange(false)) {
      Numeric n; c.checked=now;
      if (!find_numeric(target.get(),cfg.key,n) || cfg.type!=(n.integer ? 1:2)) {
        disable(c,self.get(),"Disabled — property missing, hidden, disabled, or changed type"); continue;
      }
      if (cfg.low<n.low || cfg.high>n.high) {
        disable(c,self.get(),"Disabled — modulation range exceeds target limits"); continue;
      }
      if (!c.watch) c.watch = std::make_unique<Watch>(target.get(),n);
      // Metadata refresh never changes a connected listener's immutable key/type.
      c.watch->number.low=n.low; c.watch->number.high=n.high;
    }
    auto &w=*c.watch;
    if (w.external || (w.wrote && !modulator::same_value(read_value(target.get(),w.number),w.expected,w.number.integer))) {
      disable(c,self.get(),"Paused — target changed externally; enable the eye to restart"); continue;
    }
    if (!c.active) {
      c.active=true; c.started=now; c.snapshot=read_value(target.get(),w.number);
      c.ease_start=c.snapshot;
      c.snapshot_uuid=uuid; c.snapshot_key=cfg.key; c.snapshot_integer=w.number.integer;
      c.snapshot_valid=std::isfinite(c.snapshot);
    }
    if (c.restart.exchange(false)) {
      c.started=now; c.ease_start=read_value(target.get(),w.number);
    }
    const double value=modulator::mapped(static_cast<modulator::Wave>(cfg.wave),now-c.started,
      cfg.period,cfg.low,cfg.high,w.number.integer,w.number.low,w.number.high,
      c.glider_seed,cfg.ease_in,c.ease_start);
    if (!std::isfinite(value)) { disable(c,self.get(),"Disabled — non-finite target value"); continue; }
    const double current=read_value(target.get(),w.number);
    if (!modulator::same_value(current,value,w.number.integer)) {
      auto &batch=batches[uuid];
      if (!batch.target) batch.target=own(obs_source_get_ref(target.get()));
      batch.changes.push_back({h,w.number,current,value});
    } else { w.expected=value; w.wrote=true; }
    uint64_t interval=obs_get_frame_interval_ns();
    status(c,self.get(),interval && cfg.period < 8.0*static_cast<double>(interval)/1e9
      ? "Running — fewer than 8 frames per cycle; motion may alias" : "Running — manual target edits turn this eye off");
  }
  for (auto &entry : batches) {
    auto &batch=entry.second; bool changed=false;
    for (auto &change:batch.changes) {
      auto &c=*change.controller; std::lock_guard<std::recursive_mutex> lock(c.mutex);
      auto self=own(obs_weak_source_get_source(c.self));
      if(c.dead || !self || !c.watch || c.gate_changed || !obs_source_enabled(self.get())) continue;
      auto *parent=obs_filter_get_parent(self.get());
      if(!parent || !obs_source_showing(parent) || obs_source_removed(batch.target.get())) continue;
      if(c.watch->external || !modulator::same_value(read_value(batch.target.get(),change.number),change.observed,change.number.integer)) {
        disable(c,self.get(),"Paused — target changed externally; enable the eye to restart");continue;
      }
      c.watch->write_generation.fetch_add(1);
      c.watch->expected=change.value;c.watch->wrote=true;batch.writing.push_back(c.watch.get());
      write(batch.delta,change.number,change.value);changed=true;
    }
    if(changed)obs_source_update(batch.target.get(),batch.delta.get());
    for(auto *watch:batch.writing)watch->write_generation.fetch_add(1);
  }
  pending=false;
}
void tick(void *, float)
{
  if (!stopping && !pending.exchange(true)) obs_queue_task(OBS_TASK_UI,pump,nullptr,false);
}

bool source_list(void *p, obs_source_t *s)
{
  if (!is_controller(s)) obs_property_list_add_string(static_cast<obs_property_t *>(p),obs_source_get_name(s),obs_source_get_uuid(s));
  return true;
}
void preserve_selection(obs_property_t *p,const char *value,const char *label)
{
  if(!value || !*value)return;
  for(size_t i=0;i<obs_property_list_item_count(p);++i)
    if(std::string(obs_property_list_item_string(p,i))==value)return;
  obs_property_list_add_string(p,label,value);
}
void choices(obs_properties_t *props, Controller *c, obs_data_t *d)
{
  auto self = own(c ? obs_weak_source_get_source(c->self):nullptr);
  auto *filters=obs_properties_get(props,"filter_uuid");
  auto *fields=obs_properties_get(props,"property");
  obs_property_list_clear(filters); obs_property_list_clear(fields);
  obs_property_list_add_string(filters,"Source settings","");
  obs_property_list_add_string(fields,"Choose a numeric property","");
  auto preserve=[&](){preserve_selection(filters,obs_data_get_string(d,"filter_uuid"),"Missing filter — select a replacement");
    preserve_selection(fields,obs_data_get_string(d,"property"),"Unavailable property — select a replacement");};
  if (!self) {preserve();return;}
  Config cfg=config(d); cfg.filter.clear(); auto parent=resolve(self.get(),cfg);
  if (parent) obs_source_enum_filters(parent.get(),[](obs_source_t *,obs_source_t *f,void *p) {
    if (!is_controller(f)) obs_property_list_add_string(static_cast<obs_property_t *>(p),obs_source_get_name(f),obs_source_get_uuid(f));
  },filters);
  auto target=resolve(self.get(),config(d));
  if (!target) {preserve();return;}
  auto p=properties(target.get()); std::vector<Numeric> all; numeric_properties(p.get(),all);
  for (const auto &n:all) obs_property_list_add_string(fields,n.label.c_str(),n.key.c_str());
  Numeric n;
  if (find_numeric(target.get(),obs_data_get_string(d,"property"),n)) {
    for (auto key:{"minimum","maximum"}) obs_property_float_set_limits(obs_properties_get(props,key),n.low,n.high,n.integer?1:std::max(1e-6,n.step));
  }
  preserve();
}
bool target_changed(void *priv, obs_properties_t *props, obs_property_t *p, obs_data_t *d)
{
  auto *c=static_cast<Controller *>(priv);
  if(!c)return false;
  std::lock_guard<std::recursive_mutex> lock(c->mutex);
  bool source_change=std::string(obs_property_name(p))=="source_uuid";
  const std::string selected=obs_data_get_string(d,source_change?"source_uuid":"filter_uuid");
  if(selected==(source_change?c->ui_source:c->ui_filter))return false;
  if(source_change)obs_data_set_string(d,"filter_uuid","");
  obs_data_set_string(d,"property",""); obs_data_set_int(d,"property_type",0);
  c->ui_source=obs_data_get_string(d,"source_uuid");c->ui_filter=obs_data_get_string(d,"filter_uuid");c->ui_key="";
  choices(props,c,d); return true;
}
bool property_changed(void *priv, obs_properties_t *props, obs_property_t *, obs_data_t *d)
{
  auto *c=static_cast<Controller *>(priv);
  if(!c)return false;
  std::lock_guard<std::recursive_mutex> lock(c->mutex);
  const std::string selected=obs_data_get_string(d,"property");
  if(selected==c->ui_key)return false;
  c->ui_key=selected;
  auto self=own(c ? obs_weak_source_get_source(c->self):nullptr);
  if (!self) return false;
  auto target=resolve(self.get(),config(d)); Numeric n;
  if (find_numeric(target.get(),obs_data_get_string(d,"property"),n)) {
    const double v=read_value(target.get(),n);
    obs_data_set_int(d,"property_type",n.integer?1:2);
    obs_data_set_double(d,"minimum",v);obs_data_set_double(d,"maximum",v);
  }
  choices(props,c,d);return true;
}
bool restart_clicked(obs_properties_t *, obs_property_t *, void *priv)
{ if(priv) static_cast<Controller *>(priv)->restart=true; return false; }
bool restore_clicked(obs_properties_t *, obs_property_t *, void *priv)
{ if(priv) static_cast<Controller *>(priv)->restore=true; return false; }
const char *name(void *) { return obs_module_text("FilterName"); }
obs_properties_t *get_properties(void *data)
{
  Controller *c=data ? static_cast<Handle *>(data)->get():nullptr;
  auto *props=obs_properties_create();
  auto *src=obs_properties_add_list(props,"source_uuid","Target source",OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_STRING);
  obs_property_list_add_string(src,"This source","@parent"); obs_enum_sources(source_list,src);obs_enum_scenes(source_list,src);
  auto *f=obs_properties_add_list(props,"filter_uuid","Target filter",OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_STRING);
  auto *key=obs_properties_add_list(props,"property","Target property",OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_STRING);
  obs_properties_add_float(props,"minimum","Minimum",-1e12,1e12,.01);
  obs_properties_add_float(props,"maximum","Maximum",-1e12,1e12,.01);
  auto *wave=obs_properties_add_list(props,"waveform","Waveform",OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_INT);
  const char *names[]={"Sine","Triangle","Rising saw","Falling saw","Square","Glider"};
  for(int i=0;i<6;++i)obs_property_list_add_int(wave,names[i],i);
  obs_property_set_long_description(wave,"Glider smoothly travels between random values; cycle time is the time between random destinations.");
  obs_properties_add_float(props,"period","Cycle time (seconds)",.05,86400,.05);
  auto *ease=obs_properties_add_bool(props,"ease_in","Ease in");
  obs_property_set_long_description(ease,"Gently blend from the current target value into modulation over one cycle. Re-enabling or restarting fades in again.");
  obs_properties_add_button2(props,"restart","Restart cycle",restart_clicked,c);
  obs_properties_add_button2(props,"restore","Restore starting value and disable",restore_clicked,c);
  auto *s=obs_properties_add_text(props,"status","Disabled — configure the mapping, then enable the filter eye",OBS_TEXT_INFO);
  obs_properties_add_text(props,"help","Active while the eye is enabled and this source is showing (including previews). Stops hold the last value. External target edits turn the eye off. Target dialogs may not redraw continuously.",OBS_TEXT_INFO);
  obs_property_set_modified_callback2(src,target_changed,c);obs_property_set_modified_callback2(f,target_changed,c);
  obs_property_set_modified_callback2(key,property_changed,c);
  if(c) {
    std::lock_guard<std::recursive_mutex> lock(c->mutex);
    obs_property_set_description(s,c->status.c_str());
    auto self=own(obs_weak_source_get_source(c->self));
    if(self){auto d=settings(self.get());c->ui_source=obs_data_get_string(d.get(),"source_uuid");c->ui_filter=obs_data_get_string(d.get(),"filter_uuid");c->ui_key=obs_data_get_string(d.get(),"property");preserve_selection(src,c->ui_source.c_str(),"Missing source — select a replacement");choices(props,c,d.get());}
  }
  return props;
}
void defaults(obs_data_t *d)
{
  obs_data_set_default_int(d,"schema_version",1);
  obs_data_set_default_string(d,"source_uuid","@parent");
  obs_data_set_default_double(d,"period",4);
  obs_data_set_default_bool(d,"ease_in",false);
}
void *create(obs_data_t *d,obs_source_t *self)
{
  auto h=std::make_shared<Controller>(self);
  h->initialized=obs_data_get_bool(d,"configured");
  { std::lock_guard<std::mutex> lock(registry_mutex);controllers.push_back(h); }
  return new Handle(std::move(h));
}
void filter_added(void *data,obs_source_t *)
{
  auto &c=**static_cast<Handle *>(data);
  if (!c.initialized.exchange(true)) {
    auto self=own(obs_weak_source_get_source(c.self));
    if(self){obs_source_set_enabled(self.get(),false);auto d=settings(self.get());obs_data_set_bool(d.get(),"configured",true);}
  }
}
void load(void *data,obs_data_t *)
{ static_cast<Handle *>(data)->get()->initialized=true; }
void destroy(void *data)
{
  std::unique_ptr<Handle> holder(static_cast<Handle *>(data));
  (*holder)->dead=true;
  std::lock_guard<std::mutex> lock(registry_mutex);
  controllers.erase(std::remove(controllers.begin(),controllers.end(),*holder),controllers.end());
}
void render(void *data,gs_effect_t *)
{
  auto self=own(obs_weak_source_get_source((*static_cast<Handle *>(data))->self));
  if(self)obs_source_skip_video_filter(self.get());
}
}

bool obs_module_load(void)
{
  stopping=false;
  obs_source_info info={}; info.id=ID;info.type=OBS_SOURCE_TYPE_FILTER;
  info.output_flags=OBS_SOURCE_VIDEO;info.get_name=name;info.create=create;info.destroy=destroy;
  info.get_defaults=defaults;info.get_properties=get_properties;info.video_render=render;
  info.filter_add=filter_added;info.load=load;
  obs_register_source(&info);obs_add_tick_callback(tick,nullptr);
  blog(LOG_INFO,"[obs-modulator] Loaded %s (LFO only)", MODULATOR_VERSION);return true;
}
void obs_module_unload(void)
{
  stopping=true;obs_remove_tick_callback(tick,nullptr);
  // Frontend/module shutdown must drain its UI queue before unloading code.
  obs_queue_task(OBS_TASK_UI,pump,nullptr,true);
}
