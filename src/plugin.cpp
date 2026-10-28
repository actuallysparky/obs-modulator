// SPDX-FileCopyrightText: 2026 Sparky and OBS Modulator contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include <obs-module.h>
#include <util/platform.h>
#include "lfo.hpp"
#include "monitor.hpp"
#ifdef MODULATOR_DOCK
#include "frontend.hpp"
#endif
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
bool find_numeric(obs_properties_t *props, const std::string &key, Numeric &n);
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
struct Scalar {
  obs_data_type type;
  double value;
  obs_data_number_type number_type=OBS_DATA_NUM_INVALID;
  bool operator==(const Scalar &b) const {return type==b.type && value==b.value && number_type==b.number_type;}
};
using Scalars=std::map<std::string,Scalar>;
Scalars scalar_settings(obs_data_t *d) {
  Scalars result;
  for(auto *item=obs_data_first(d);item;obs_data_item_next(&item)) {
    const auto type=obs_data_item_gettype(item);
    if(type==OBS_DATA_NUMBER || type==OBS_DATA_BOOLEAN)
      result.emplace(obs_data_item_get_name(item),Scalar{type,type==OBS_DATA_BOOLEAN ?
        double(obs_data_item_get_bool(item)):obs_data_item_get_double(item),obs_data_item_numtype(item)});
  }
  return result;
}
void set_scalar(obs_data_t *d,const std::string &key,const Scalar &s) {
  if(s.type==OBS_DATA_BOOLEAN)obs_data_set_bool(d,key.c_str(),s.value!=0);
  else if(s.number_type==OBS_DATA_NUM_INT)obs_data_set_int(d,key.c_str(),static_cast<long long>(s.value));
  else obs_data_set_double(d,key.c_str(),s.value);
}

// A numeric property can be a UI action as well as a stored setting. Keep its
// native callback context alive between frames, shared by mappings of a target.
// Construct it BEFORE changing settings: some targets capture the old value in
// get_properties. Refresh on metadata signals, callback requests, or our normal
// one-second metadata check, never by rebuilding an open Qt property dialog.
struct PropertyContext {
  obs_weak_source_t *target;
  Props props{nullptr,obs_properties_destroy};
  std::atomic<bool> dirty{true};
  double checked = 0;
  uint64_t generation = 0;
  static void changed(void *p,calldata_t *) {static_cast<PropertyContext *>(p)->dirty=true;}
  explicit PropertyContext(obs_source_t *s):target(obs_source_get_weak_source(s)) {
    signal_handler_connect(obs_source_get_signal_handler(s),"update_properties",changed,this);
  }
  ~PropertyContext() {
    auto s=own(obs_weak_source_get_source(target));
    if(s)signal_handler_disconnect(obs_source_get_signal_handler(s.get()),"update_properties",changed,this);
    obs_weak_source_release(target);
  }
  void refresh(obs_source_t *s,double now) {
    const bool invalid=dirty.exchange(false);
    if(!props || invalid || now-checked>=1) {
      props=properties(s);checked=now;++generation;
    }
  }
};
std::map<std::string,std::weak_ptr<PropertyContext>> property_contexts;
std::shared_ptr<PropertyContext> property_context(obs_source_t *s,double now) {
  auto &weak=property_contexts[obs_source_get_uuid(s)];auto p=weak.lock();
  if(!p){p=std::make_shared<PropertyContext>(s);weak=p;}
  p->refresh(s,now);return p;
}

struct Watch {
  obs_weak_source_t *target;
  Numeric number;
  std::shared_ptr<PropertyContext> context;
  uint64_t property_generation = 0;
  std::atomic<double> expected{0};
  std::atomic<uint64_t> write_generation{0};
  std::atomic<bool> wrote{false}, external{false}, dirty{false};
  std::shared_ptr<const Scalars> dependents=std::make_shared<const Scalars>();
  bool matches(obs_source_t *source) const {
    auto d=settings(source);auto expected_dependents=std::atomic_load(&dependents);
    if(!modulator::same_value(read_value(source,number),expected,number.integer))return false;
    for(const auto &entry:*expected_dependents) {
      auto *item=obs_data_item_byname(d.get(),entry.first.c_str());
      if(!item)return false;
      const auto type=obs_data_item_gettype(item);
      const Scalar current{type,type==OBS_DATA_BOOLEAN ? double(obs_data_item_get_bool(item)):obs_data_item_get_double(item),obs_data_item_numtype(item)};
      obs_data_item_release(&item);
      if(!(current==entry.second))return false;
    }
    return true;
  }
  static void changed(void *param, calldata_t *)
  {
    auto *w = static_cast<Watch *>(param);
    const uint64_t before=w->write_generation.load();
    if(before & 1) return;
    auto target = own(obs_weak_source_get_source(w->target));
    if(!target || !w->wrote)return;
    const bool matches=w->matches(target.get());
    if(before!=w->write_generation.load())return;
    if(!matches)w->external=true;
  }
  static void schema(void *param, calldata_t *) { static_cast<Watch *>(param)->dirty = true; }
  Watch(obs_source_t *source, Numeric n, std::shared_ptr<PropertyContext> p)
    : target(obs_source_get_weak_source(source)), number(std::move(n)), context(std::move(p)),
      property_generation(context->generation)
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
  bool ease_in = false, ease_out = false, one_shot = false;
  bool operator==(const Config &b) const {
    return source==b.source && filter==b.filter && key==b.key && type==b.type &&
      wave==b.wave && low==b.low && high==b.high && period==b.period && ease_in==b.ease_in && ease_out==b.ease_out && one_shot==b.one_shot;
  }
};
Config config(obs_data_t *d)
{
  return {obs_data_get_string(d,"source_uuid"), obs_data_get_string(d,"filter_uuid"),
    obs_data_get_string(d,"property"), static_cast<int>(obs_data_get_int(d,"property_type")),
    static_cast<int>(obs_data_get_int(d,"waveform")), obs_data_get_double(d,"minimum"),
    obs_data_get_double(d,"maximum"), obs_data_get_double(d,"period"), obs_data_get_bool(d,"ease_in"),
    obs_data_has_user_value(d,"ease_out") ? obs_data_get_bool(d,"ease_out") : obs_data_get_bool(d,"ease_in"),
    obs_data_get_bool(d,"one_shot")};
}
struct Controller {
  enum class StopReason { None, ExternalEdit, Invalid, Completed, Cancelled };
  obs_weak_source_t *self;
  std::recursive_mutex mutex;
  std::atomic<bool> dead{false}, restart{false}, restore{false}, play{false};
  std::atomic<uint64_t> revision{0}, disabled_revision{0};
  std::atomic<bool> gate_changed{false};
  std::atomic<bool> initialized{false};
  Config last;
  std::shared_ptr<Watch> watch;
  bool active = false, releasing = false, snapshot_valid = false, natural_return = false, armed = false;
  uint64_t shot_index = 0, shot_seed = 0;
  bool play_value_valid = false;
  double play_value = 0;
  uint64_t play_revision = 0;
  std::string play_uuid, play_key;
  double started = 0, checked = 0, snapshot = 0, ease_start = 0;
  double release_started = 0, release_from = 0;
  uint64_t glider_seed = UINT64_C(14695981039346656037);
  std::string snapshot_uuid, snapshot_key, status = "Disabled — choose a target, then enable the eye";
  std::string identity, ui_source, ui_filter, ui_key;
  bool snapshot_integer = false;
  StopReason stop_reason = StopReason::None;
  static void enabled(void *p, calldata_t *event) {
    auto &c=*static_cast<Controller *>(p);const auto revision=++c.revision;c.gate_changed=true;
    if(!calldata_bool(event,"enabled"))c.disabled_revision=revision;
  }
  explicit Controller(obs_source_t *s) : self(obs_source_get_weak_source(s)), identity(obs_source_get_uuid(s))
  {
    // The saved filter UUID gives each mapping independent, reproducible motion.
    for (unsigned char ch : identity) { glider_seed ^= ch; glider_seed *= UINT64_C(1099511628211); }
    shot_seed=glider_seed ^ os_gettime_ns();
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
  auto p = properties(target);return find_numeric(p.get(),key,n);
}
bool find_numeric(obs_properties_t *props, const std::string &key, Numeric &n)
{
  std::vector<Numeric> all;numeric_properties(props, all);
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
void disable(Controller &c, obs_source_t *self, const char *why,
             Controller::StopReason reason = Controller::StopReason::Invalid)
{
  c.stop_reason = reason;
  c.active = false; c.releasing = false; c.natural_return=false; c.watch.reset();
  c.restart=false; c.play=false; c.play_value_valid=false; c.armed=false; ++c.revision;
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
  struct Change { Handle controller; Numeric number; double observed, value; uint64_t revision; bool finish; bool applied=false; };
  std::vector<Change> changes;
  std::vector<std::shared_ptr<Watch>> writing;
};
#ifdef MODULATOR_TESTING
static double test_clock = -1;
#endif
double engine_now()
{
#ifdef MODULATOR_TESTING
  if(test_clock>=0)return test_clock;
#endif
  return static_cast<double>(os_gettime_ns())/1e9;
}
void finish(Controller &c, obs_source_t *self, const Config &cfg)
{
  if(cfg.one_shot) {
    const bool complete=c.natural_return || obs_source_enabled(self);
    disable(c,self,complete ? "Completed — holding final value; Play to run again"
                           : "Cancelled — holding final value; Play to run again",
      complete ? Controller::StopReason::Completed : Controller::StopReason::Cancelled);
  } else {
    c.active=false; c.releasing=false; c.watch.reset();
    status(c,self,obs_source_enabled(self) ? "Waiting — eased back to starting value; parent is hidden"
                                        : "Disabled — eased back to starting value");
  }
}
void pump(void *)
{
  // One frame-paced UI task, partial writes batched by target. No shot timers.
  if(stopping){property_contexts.clear();pending=false;return;}
  for(auto i=property_contexts.begin();i!=property_contexts.end();)
    if(i->second.expired())i=property_contexts.erase(i);else ++i;
  std::vector<Handle> list;
  {std::lock_guard<std::mutex> lock(registry_mutex);list=controllers;}
  // Resolve configuration once per controller, not once per sort comparison.
  std::map<Controller *,bool> established;
  for(auto &h:list){std::lock_guard<std::recursive_mutex> lock(h->mutex);
    auto self=own(obs_weak_source_get_source(h->self));
    auto d=self ? settings(self.get()):Data(nullptr,obs_data_release);
    established[h.get()]=self && h->active && config(d.get())==h->last;}
  std::sort(list.begin(),list.end(),[&](const Handle &a,const Handle &b){
    const bool aa=established.at(a.get()),bb=established.at(b.get());
    return aa!=bb ? aa>bb : a->identity<b->identity;
  });
  std::map<std::string,Controller *> owners;
  std::map<std::string,Batch> batches;
  const double now=engine_now();
  for(auto &h:list) {
    auto &c=*h;std::lock_guard<std::recursive_mutex> lock(c.mutex);
    if(c.dead)continue;
    auto self=own(obs_weak_source_get_source(c.self));
    if(!self || obs_source_removed(self.get()))continue;
    const bool gate_changed=c.gate_changed.exchange(false);
    // A later eye-off cancels an older Play, even if off/on coalesced in one frame.
    if(c.disabled_revision>c.play_revision){c.play=false;c.play_value_valid=false;}
    auto d=settings(self.get());const Config cfg=config(d.get());
    if(c.restore.exchange(false)) {
      disable(c,self.get(),"Disabled — restored starting value",Controller::StopReason::None);
      auto target=resolve(self.get(),cfg);Numeric n;
      if(c.snapshot_valid && target && c.snapshot_uuid==obs_source_get_uuid(target.get()) &&
         c.snapshot_key==cfg.key && find_numeric(target.get(),cfg.key,n) && n.integer==c.snapshot_integer) {
        auto context=property_context(target.get(),now);auto current=settings(target.get());
        write(current,n,std::clamp(c.snapshot,n.low,n.high));
        if(obs_property_modified(obs_properties_get(context->props.get(),n.key.c_str()),current.get()))context->dirty=true;
        if(!obs_source_removed(target.get()))obs_source_update(target.get(),nullptr);
      } else {c.stop_reason=Controller::StopReason::Invalid;status(c,self.get(),"Disabled — starting value unavailable for this target");}
      continue;
    }
    if(!(cfg==c.last)) {
      const bool cancel=(cfg.one_shot || c.last.one_shot) &&
        (c.active || (obs_source_enabled(self.get()) && !gate_changed && !c.play));
      c.active=false;c.releasing=false;c.natural_return=false;c.watch.reset();c.last=cfg;++c.revision;
      if(cancel){disable(c,self.get(),"Cancelled — configuration changed; Play to run again",Controller::StopReason::Cancelled);continue;}
    }
    auto *parent=obs_filter_get_parent(self.get());
    bool enabled=obs_source_enabled(self.get());
    bool requested=enabled && parent && obs_source_showing(parent);
    if(cfg.one_shot && gate_changed && enabled && !c.active){c.armed=true;c.stop_reason=Controller::StopReason::None;}
    // Check the old watch BEFORE processing a queued Play/restart/eye edge.
    auto watched=own(c.watch ? obs_weak_source_get_source(c.watch->target):nullptr);
    if(c.watch && watched && (c.watch->external || (c.watch->wrote && !c.watch->matches(watched.get())))) {
      disable(c,self.get(),"Paused — target changed externally; enable the eye to restart",Controller::StopReason::ExternalEdit);continue;
    }
    const bool play=c.play.exchange(false);
    bool retrigger=play || (cfg.one_shot && gate_changed && enabled && c.active);
    if(gate_changed && enabled && !cfg.one_shot) {c.active=false;c.releasing=false;c.watch.reset();}
    if(cfg.one_shot && c.active && !requested && enabled) {
      // An interrupted shot cannot replay merely because the parent reappears.
      obs_source_set_enabled(self.get(),false);c.gate_changed=false;enabled=false;
    }
    if(retrigger && c.active) {c.releasing=false;c.natural_return=false;}
    if(!requested && (!parent || !c.active || !cfg.ease_out || !c.snapshot_valid)) {
      const bool cancelled=cfg.one_shot && (c.active || (!enabled && c.armed));
      c.active=false;c.releasing=false;c.natural_return=false;c.watch.reset();c.restart=false;
      if(cancelled){disable(c,self.get(),"Cancelled — holding last value; Play to run again",Controller::StopReason::Cancelled);}
      else if(enabled)status(c,self.get(),"Waiting — parent source is hidden");
      else if(c.stop_reason==Controller::StopReason::None &&
        (c.status.rfind("Running",0)==0 || c.status.rfind("Waiting",0)==0 || c.status.rfind("Easing",0)==0))
        status(c,self.get(),"Disabled — holding the last value");
      continue;
    }
    if(cfg.key.empty()){disable(c,self.get(),"Disabled — choose a numeric target");continue;}
    if(!std::isfinite(cfg.low) || !std::isfinite(cfg.high) || cfg.low>cfg.high ||
       !std::isfinite(cfg.period) || cfg.period<0.05 || cfg.period>86400 || cfg.wave<0 || cfg.wave>=modulator::wave_count) {
      disable(c,self.get(),"Disabled — invalid range, waveform, or cycle time");continue;
    }
    auto target=resolve(self.get(),cfg);
    if(!target){disable(c,self.get(),"Disabled — target was removed; select it again");continue;}
    const std::string uuid=obs_source_get_uuid(target.get());
    if(c.active && c.snapshot_uuid!=uuid){disable(c,self.get(),"Disabled — target identity changed; enable the eye to restart");continue;}
    const std::string ownership=uuid+"/"+cfg.key;
    if(owners.count(ownership)){disable(c,self.get(),"Disabled — another LFO owns this property");continue;}
    owners[ownership]=&c;
    if(c.watch)for(const auto &dependent:*std::atomic_load(&c.watch->dependents))
      owners.emplace(uuid+"/"+dependent.first,&c);
    auto context=property_context(target.get(),now);
    if(!c.watch || now-c.checked>=1 || c.watch->dirty.exchange(false) || c.watch->property_generation!=context->generation) {
      Numeric n;c.checked=now;
      if(!find_numeric(context->props.get(),cfg.key,n) || cfg.type!=(n.integer?1:2)) {
        disable(c,self.get(),"Disabled — property missing, hidden, disabled, or changed type");continue;
      }
      if(cfg.low<n.low || cfg.high>n.high){disable(c,self.get(),"Disabled — modulation range exceeds target limits");continue;}
      if(!c.watch)c.watch=std::make_shared<Watch>(target.get(),n,context);
      c.watch->property_generation=context->generation;
      c.watch->number.low=n.low;c.watch->number.high=n.high;
    }
    auto &w=*c.watch;
    if(requested && c.releasing && !c.natural_return && !cfg.one_shot){c.active=false;c.releasing=false;}
    const double current=read_value(target.get(),w.number);
    if(c.play_value_valid) {
      const bool external=c.play_uuid==uuid && c.play_key==cfg.key &&
        !modulator::same_value(current,c.play_value,w.number.integer);
      c.play_value_valid=false;
      if(external){disable(c,self.get(),"Paused — target changed after Play; operator value preserved",Controller::StopReason::ExternalEdit);continue;}
    }
    if(!c.active) {
      c.stop_reason=Controller::StopReason::None;c.armed=false;c.active=true;c.started=now;c.snapshot=current;c.ease_start=current;
      c.snapshot_uuid=uuid;c.snapshot_key=cfg.key;c.snapshot_integer=w.number.integer;c.snapshot_valid=std::isfinite(current);
      c.releasing=false;c.natural_return=false;
      if(cfg.one_shot)++c.shot_index;
      retrigger=false;
    }
    if(requested && (c.restart.exchange(false) || retrigger)) {
      c.started=now;c.ease_start=current;c.releasing=false;c.natural_return=false;
      if(cfg.one_shot)++c.shot_index;
    }
    const auto wave=static_cast<modulator::Wave>(cfg.wave);
    auto shot=[&](double elapsed){return modulator::shot_mapped(wave,elapsed,cfg.period,cfg.low,cfg.high,
      w.number.integer,w.number.low,w.number.high,c.shot_seed,cfg.ease_in,c.ease_start,c.shot_index);};
    if(!requested) {
      c.restart=false;
      if(!c.releasing){c.releasing=true;c.release_started=now;c.release_from=current;}
      c.natural_return=false;
    } else if(cfg.one_shot && !c.releasing && now-c.started>=cfg.period && cfg.ease_out) {
      c.releasing=true;c.natural_return=true;c.release_started=c.started+cfg.period;c.release_from=shot(cfg.period);
    }
    const double value=c.releasing ? modulator::eased_return(now-c.release_started,cfg.period,c.release_from,c.snapshot,
        w.number.integer,w.number.low,w.number.high)
      : cfg.one_shot ? shot(now-c.started)
      : modulator::mapped(wave,now-c.started,cfg.period,cfg.low,cfg.high,w.number.integer,
          w.number.low,w.number.high,c.glider_seed,cfg.ease_in,c.ease_start);
    if(!std::isfinite(value)){disable(c,self.get(),"Disabled — non-finite target value");continue;}
    const bool ending=c.releasing ? now-c.release_started>=cfg.period : cfg.one_shot && now-c.started>=cfg.period;
    if((ending && current!=value) || !modulator::same_value(current,value,w.number.integer)) {
      auto &batch=batches[uuid];if(!batch.target)batch.target=own(obs_source_get_ref(target.get()));
      batch.changes.push_back({h,w.number,current,value,c.revision.load(),ending});
    } else {
      w.expected=value;w.wrote=true;
      if(ending){finish(c,self.get(),cfg);continue;}
    }
    const uint64_t interval=obs_get_frame_interval_ns();
    status(c,self.get(),c.releasing ? "Easing out — returning to starting value; manual edits cancel"
      : interval && cfg.period<8.0*static_cast<double>(interval)/1e9
      ? "Running — fewer than 8 frames per cycle; motion may alias" : "Running — manual target edits turn this eye off");
  }
  for(auto &entry:batches) {
    auto &batch=entry.second;bool changed=false;
    if(obs_source_removed(batch.target.get()))continue;
    auto context=property_context(batch.target.get(),now);
    auto current=settings(batch.target.get());
    // A callback can submit its own synchronous update. Guard all watched
    // fields of this target during the transaction, then verify readback. This
    // avoids mistaking a callback's temporary overlapping writes for operator
    // edits while enforcing the established owner's dependency footprint.
    for(auto &h:list) {
      std::lock_guard<std::recursive_mutex> lock(h->mutex);
      auto target=own(h->watch ? obs_weak_source_get_source(h->watch->target):nullptr);
      if(target.get()==batch.target.get()) {
        h->watch->write_generation.fetch_add(1);batch.writing.push_back(h->watch);
      }
    }
    for(auto &change:batch.changes) {
      auto &c=*change.controller;std::lock_guard<std::recursive_mutex> lock(c.mutex);
      auto self=own(obs_weak_source_get_source(c.self));
      if(c.dead || !self || !c.watch || c.gate_changed || c.revision!=change.revision || c.watch->dirty || c.restart || c.play || c.restore)continue;
      auto *parent=obs_filter_get_parent(self.get());
      const bool requested=obs_source_enabled(self.get()) && parent && obs_source_showing(parent);
      if(!parent || (!c.releasing && !requested) || (c.releasing && requested && !c.natural_return) || obs_source_removed(batch.target.get()))continue;
      auto d=settings(self.get());if(!(config(d.get())==c.last))continue;
      if(c.watch->external || (c.watch->wrote && !c.watch->matches(batch.target.get())) ||
         !modulator::same_value(read_value(batch.target.get(),change.number),change.observed,change.number.integer)) {
        disable(c,self.get(),"Paused — target changed externally; enable the eye to restart",Controller::StopReason::ExternalEdit);continue;
      }
      Numeric accepted;
      if(!find_numeric(context->props.get(),change.number.key,accepted) || accepted.integer!=change.number.integer ||
         change.value<accepted.low || change.value>accepted.high) {
        disable(c,self.get(),"Disabled — target property changed before its edit");continue;
      }
      auto watch=c.watch;watch->expected=change.value;watch->wrote=true;change.applied=true;
      // Edit only this key in the CURRENT settings, then run its native OBS
      // callback. Dependent changes made by that callback are part of the edit;
      // never reapply a stale full settings snapshot or call unrelated callbacks.
      const auto before=scalar_settings(current.get());
      write(current,change.number,change.value);changed=true;
      if(obs_property_modified(obs_properties_get(context->props.get(),change.number.key.c_str()),current.get()))context->dirty=true;
      auto dependencies=std::make_shared<Scalars>(*std::atomic_load(&watch->dependents));
      bool conflict=false;
      for(const auto &value:scalar_settings(current.get())) {
        if(value.first==change.number.key)continue;
        auto old=before.find(value.first);if(old!=before.end() && old->second==value.second)continue;
        auto owner=owners.find(entry.first+"/"+value.first);
        if(owner!=owners.end() && owner->second!=&c) {
          // Preserve the earlier owner; cancel this compound control instead
          // of letting two callbacks fight over the derived numeric setting.
          if(old!=before.end())set_scalar(current.get(),value.first,old->second);
          conflict=true;
        } else dependencies->insert_or_assign(value.first,value.second);
      }
      if(conflict && !c.dead && c.watch==watch) {
        disable(c,self.get(),"Disabled — this control changes a property owned by another LFO");continue;
      }
      std::atomic_store(&watch->dependents,std::shared_ptr<const Scalars>(dependencies));
      for(const auto &dependent:*dependencies)owners.emplace(entry.first+"/"+dependent.first,&c);
    }
    // Publish once per target after callbacks, also for synchronous/non-video
    // targets. A target callback can itself submit updates, just as in OBS's UI.
    if(changed && !obs_source_removed(batch.target.get()))obs_source_update(batch.target.get(),nullptr);
    for(auto &watch:batch.writing)watch->write_generation.fetch_add(1);
    for(auto &change:batch.changes) {
      auto &c=*change.controller;std::lock_guard<std::recursive_mutex> lock(c.mutex);
      if(!change.applied || c.dead || !c.watch || c.revision!=change.revision || c.watch->external)continue;
      auto self=own(obs_weak_source_get_source(c.self));
      if(self && obs_source_removed(batch.target.get())) {
        disable(c,self.get(),"Disabled — target was removed during its edit");continue;
      }
      if(self && !c.watch->matches(batch.target.get())) {
        disable(c,self.get(),"Paused — target adjusted the edit or a related value; holding its values",Controller::StopReason::ExternalEdit);continue;
      }
      if(change.finish && self && !c.watch->dirty && c.watch->wrote && c.watch->expected==change.value && read_value(batch.target.get(),change.number)==change.value)
        finish(c,self.get(),c.last);
    }
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
bool request_play(Controller &c, obs_source_t *self)
{
  if(c.dead || !self || obs_source_removed(self) || !obs_filter_get_parent(self))return false;
  auto d=settings(self);const Config cfg=config(d.get());if(!cfg.one_shot)return false;
  auto target=resolve(self,cfg);Numeric n;
  c.play_value_valid=target && find_numeric(target.get(),cfg.key,n) && cfg.type==(n.integer ? 1:2);
  if(c.play_value_valid){c.play_value=read_value(target.get(),n);c.play_uuid=obs_source_get_uuid(target.get());c.play_key=cfg.key;}
  c.play_revision=++c.revision;c.play=true;c.armed=true;obs_source_set_enabled(self,true);return true;
}
bool play_clicked(obs_properties_t *, obs_property_t *, void *priv)
{
  if(priv){auto &c=*static_cast<Controller *>(priv);std::lock_guard<std::recursive_mutex> lock(c.mutex);
    auto self=own(obs_weak_source_get_source(c.self));request_play(c,self.get());}
  return false;
}
bool restart_clicked(obs_properties_t *, obs_property_t *, void *priv)
{ if(priv){auto &c=*static_cast<Controller *>(priv);++c.revision;c.restart=true;} return false; }
bool restore_clicked(obs_properties_t *, obs_property_t *, void *priv)
{ if(priv){auto &c=*static_cast<Controller *>(priv);++c.revision;c.restore=true;} return false; }
bool playback_choices(void *, obs_properties_t *props, obs_property_t *, obs_data_t *d)
{
  const Config cfg=config(d);
  obs_property_set_visible(obs_properties_get(props,"play"),cfg.one_shot);
  obs_property_set_visible(obs_properties_get(props,"restart"),!cfg.one_shot);
  obs_property_set_enabled(obs_properties_get(props,"ease_in"),!(cfg.one_shot && cfg.wave==5));
  obs_property_set_description(obs_properties_get(props,"duration"),cfg.one_shot ?
    ("Total shot: "+std::to_string(cfg.period*(cfg.ease_out ? 2:1))+" seconds. Ease in shares the first cycle.").c_str()
    : "Continuous modulation; no total completion time.");
  obs_property_set_description(obs_properties_get(props,"ease_in_help"),cfg.one_shot && cfg.wave==5 ?
    "Glider always starts smoothly at the current value. Ease in has no effect here; Ease out can still return to the activation value." :
    "Ease in blends the current value into the moving waveform during its first cycle. Ease out adds a separate one-cycle return on stop. Manual edits always cancel immediately.");
  return true;
}
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
  for(int i=0;i<modulator::wave_count;++i)obs_property_list_add_int(wave,modulator::wave_name(i),i);
  obs_property_set_long_description(wave,"Glider travels smoothly to random values. Exponential, logarithmic and S curves sweep the range; continuous ramps reset like a saw. Falling curves reverse the rising journey in time.");
  obs_properties_add_float(props,"period","Cycle time (seconds)",.05,86400,.05);
  obs_properties_add_bool(props,"one_shot","One shot");
  obs_properties_add_bool(props,"ease_in","Ease in");
  obs_properties_add_bool(props,"ease_out","Ease out");
  obs_properties_add_text(props,"ease_in_help","",OBS_TEXT_INFO);
  obs_properties_add_text(props,"duration","",OBS_TEXT_INFO);
  obs_properties_add_button2(props,"play","Play / retrigger",play_clicked,c);
  obs_properties_add_button2(props,"restart","Restart cycle",restart_clicked,c);
  obs_properties_add_button2(props,"restore","Restore starting value and disable",restore_clicked,c);
  auto *s=obs_properties_add_text(props,"status","Disabled — configure the mapping, then enable the filter eye",OBS_TEXT_INFO);
  obs_properties_add_text(props,"help","Enable the eye or Play to run while the parent is showing (including previews). One shot stops at its endpoint; Ease out optionally returns home. Hiding interrupts a shot. Operator control always wins: target edits turn the eye off and cancel queued work. Target dialogs may not redraw continuously.",OBS_TEXT_INFO);
  obs_property_set_modified_callback2(src,target_changed,c);obs_property_set_modified_callback2(f,target_changed,c);
  obs_property_set_modified_callback2(key,property_changed,c);
  for(const char *field:{"one_shot","waveform","ease_out","period"})
    obs_property_set_modified_callback2(obs_properties_get(props,field),playback_choices,c);
  auto initial=Data(obs_data_create(),obs_data_release);
  obs_data_set_double(initial.get(),"period",4);playback_choices(c,props,nullptr,initial.get());
  if(c) {
    std::lock_guard<std::recursive_mutex> lock(c->mutex);
    obs_property_set_description(s,c->status.c_str());
    auto self=own(obs_weak_source_get_source(c->self));
    if(self){auto d=settings(self.get());c->ui_source=obs_data_get_string(d.get(),"source_uuid");c->ui_filter=obs_data_get_string(d.get(),"filter_uuid");c->ui_key=obs_data_get_string(d.get(),"property");preserve_selection(src,c->ui_source.c_str(),"Missing source — select a replacement");choices(props,c,d.get());playback_choices(c,props,nullptr,d.get());}
  }
  return props;
}
void defaults(obs_data_t *d)
{
  obs_data_set_default_int(d,"schema_version",1);
  obs_data_set_default_string(d,"source_uuid","@parent");
  obs_data_set_default_double(d,"period",4);
  obs_data_set_default_bool(d,"ease_in",false);
  obs_data_set_default_bool(d,"ease_out",false);
  obs_data_set_default_bool(d,"one_shot",false);
}
void migrate(obs_data_t *d)
{
  // Explicit false is authoritative. Old combined ease_in maps to both choices.
  if(!obs_data_has_user_value(d,"ease_out"))obs_data_set_bool(d,"ease_out",obs_data_get_bool(d,"ease_in"));
  obs_data_set_int(d,"easing_version",2);
}
void update(void *, obs_data_t *d) { migrate(d); }
void *create(obs_data_t *d,obs_source_t *self)
{
  migrate(d);
  auto h=std::make_shared<Controller>(self);h->last=config(d);
  h->initialized=obs_data_get_bool(d,"configured");
  { std::lock_guard<std::mutex> lock(registry_mutex);controllers.push_back(h); }
  return new Handle(std::move(h));
}
void filter_added(void *data,obs_source_t *)
{
  auto &c=**static_cast<Handle *>(data);
  if (!c.initialized.exchange(true) || c.last.one_shot) {
    auto self=own(obs_weak_source_get_source(c.self));
    if(self){obs_source_set_enabled(self.get(),false);auto d=settings(self.get());obs_data_set_bool(d.get(),"configured",true);}
  }
}
void load(void *data,obs_data_t *)
{
  auto &c=**static_cast<Handle *>(data);c.initialized=true;
  auto self=own(obs_weak_source_get_source(c.self));
  if(self){auto d=settings(self.get());migrate(d.get());if(config(d.get()).one_shot)
    disable(c,self.get(),"Disabled — one shot is idle after loading; Play to run",Controller::StopReason::None);}
}
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

namespace modulator {
namespace {
// These caches/signals exist only during a visible monitoring session. They
// never submit settings and cannot participate in the engine's ownership watch.
struct CachedProperty {
  obs_weak_source_t *target;
  Numeric number;
  bool valid = false;
  std::atomic<bool> dirty{true};
  std::string signature;
  static void schema(void *p, calldata_t *) { static_cast<CachedProperty *>(p)->dirty=true; }
  CachedProperty(obs_source_t *s, std::string sig) : target(obs_source_get_weak_source(s)), signature(std::move(sig))
  { signal_handler_connect(obs_source_get_signal_handler(s),"update_properties",schema,this); }
  ~CachedProperty() {
    auto s=own(obs_weak_source_get_source(target));
    if(s)signal_handler_disconnect(obs_source_get_signal_handler(s.get()),"update_properties",schema,this);
    obs_weak_source_release(target);
  }
};
class ControllerMonitor final : public MonitorFeed {
  std::map<std::string,std::unique_ptr<CachedProperty>> cache;
public:
  void clear() override { cache.clear(); }
  bool set_enabled(const std::string &id, bool enabled) override {
    if(stopping)return false;
    std::vector<Handle> list;
    {std::lock_guard<std::mutex> lock(registry_mutex);list=controllers;}
    for(auto &h:list) {
      std::lock_guard<std::recursive_mutex> lock(h->mutex);
      if(h->identity!=id || h->dead)continue;
      auto self=own(obs_weak_source_get_source(h->self));
      if(!self || obs_source_removed(self.get()) || !obs_filter_get_parent(self.get()))return false;
      obs_source_set_enabled(self.get(),enabled);
      return obs_source_enabled(self.get())==enabled;
    }
    return false;
  }
  bool play(const std::string &id) override {
    if(stopping)return false;
    std::vector<Handle> list;{std::lock_guard<std::mutex> lock(registry_mutex);list=controllers;}
    for(auto &h:list){std::lock_guard<std::recursive_mutex> lock(h->mutex);
      if(h->identity==id){auto self=own(obs_weak_source_get_source(h->self));return request_play(*h,self.get());}}
    return false;
  }
  std::vector<MonitorRow> sample() override {
    if(stopping){clear();return {};}
    std::vector<Handle> list;
    {std::lock_guard<std::mutex> lock(registry_mutex);list=controllers;}
    std::vector<MonitorRow> rows;
    std::vector<std::string> live;
    const double now=engine_now();
    for(auto &h:list) {
      std::lock_guard<std::recursive_mutex> lock(h->mutex);
      if(h->dead)continue;
      auto self=own(obs_weak_source_get_source(h->self));
      auto parent=own(self ? obs_source_get_ref(obs_filter_get_parent(self.get())):nullptr);
      if(!self || obs_source_removed(self.get()) || !parent || obs_source_removed(parent.get()))continue;
      auto d=settings(self.get());const Config cfg=config(d.get());
      MonitorRow row;row.id=h->identity;row.source=obs_source_get_name(parent.get());row.name=obs_source_get_name(self.get());
      row.enabled=obs_source_enabled(self.get());row.low=cfg.low;row.high=cfg.high;row.waveform=cfg.wave;
      row.property=cfg.key;row.reason=h->status;row.integer=cfg.type==1;row.one_shot=cfg.one_shot;
      const std::string sig=cfg.source+"/"+cfg.filter+"/"+cfg.key+"/"+std::to_string(cfg.type)+"/"+obs_source_get_uuid(parent.get());
      auto it=cache.find(row.id);Source target=own(nullptr);
      if(it!=cache.end() && it->second->signature==sig)target=own(obs_weak_source_get_source(it->second->target));
      if(target && (obs_source_removed(target.get()) ||
          (!cfg.filter.empty() && (!obs_filter_get_parent(target.get()) ||
            std::string(obs_source_get_uuid(obs_filter_get_parent(target.get())))!=
              (cfg.source=="@parent" ? obs_source_get_uuid(parent.get()):cfg.source)))))target.reset();
      if(!target) {
        cache.erase(row.id);target=resolve(self.get(),cfg);
        if(target)cache[row.id]=std::make_unique<CachedProperty>(target.get(),sig);
      }
      if(target) {
        auto &entry=*cache.at(row.id);
        if(entry.dirty.exchange(false))entry.valid=find_numeric(target.get(),cfg.key,entry.number) &&
            cfg.type==(entry.number.integer ? 1:2);
        row.target=obs_source_get_name(target.get());
        if(entry.valid) {
          row.property=entry.number.label;row.integer=entry.number.integer;
          row.value=read_value(target.get(),entry.number);row.available=std::isfinite(row.value);
        }
      } else row.target="Missing target";
      const bool compatible=cfg==h->last;
      if(compatible && h->releasing && !(row.enabled && h->gate_changed))row.state=MonitorState::EasingOut;
      else if(row.enabled) {
        if(!obs_source_showing(parent.get()))row.state=MonitorState::Waiting;
        else if(!compatible || !h->active || h->gate_changed)row.state=MonitorState::Starting;
        else if(cfg.ease_in && !(cfg.one_shot && cfg.wave==5) && now-h->started<cfg.period)row.state=MonitorState::EasingIn;
        else row.state=MonitorState::Running;
      } else if(compatible && h->stop_reason==Controller::StopReason::ExternalEdit)row.state=MonitorState::Paused;
      else if(compatible && h->stop_reason==Controller::StopReason::Invalid)row.state=MonitorState::Error;
      if(!row.enabled && compatible && h->stop_reason==Controller::StopReason::Completed)row.state=MonitorState::Completed;
      if(!row.enabled && compatible && h->stop_reason==Controller::StopReason::Cancelled)row.state=MonitorState::Cancelled;
      if(compatible && h->active && cfg.period>0) {
        row.timed=true;
        const double elapsed=std::max(0.0,now-(h->releasing ? h->release_started:h->started));
        row.phase_remaining=std::max(0.0,cfg.period-elapsed);
        row.progress=h->releasing || cfg.one_shot || row.state==MonitorState::EasingIn ?
          std::clamp(elapsed/cfg.period,0.0,1.0):std::fmod(elapsed/cfg.period,1.0);
        if(!cfg.one_shot && !h->releasing && row.state==MonitorState::Running)
          row.phase_remaining=cfg.period*(1.0-row.progress);
        row.total_remaining=cfg.one_shot ? row.phase_remaining+(!h->releasing && cfg.ease_out ? cfg.period:0):0;
      }
      if(!row.available && !cfg.key.empty())row.state=MonitorState::Error;
      if(!row.available && !cfg.key.empty())row.reason=target ?
        "Target property is unavailable or changed type; select the property again" :
        "Target source/filter is missing; select a replacement";
      live.push_back(row.id);rows.push_back(std::move(row));
    }
    for(auto i=cache.begin();i!=cache.end();) {
      if(std::find(live.begin(),live.end(),i->first)==live.end())i=cache.erase(i);else ++i;
    }
    std::sort(rows.begin(),rows.end(),[](const MonitorRow &a,const MonitorRow &b){
      if(a.source!=b.source)return a.source<b.source;
      if(a.name!=b.name)return a.name<b.name;
      return a.id<b.id;
    });
    return rows;
  }
};
}
std::unique_ptr<MonitorFeed> make_monitor_feed() { return std::make_unique<ControllerMonitor>(); }
#ifdef MODULATOR_TESTING
void monitor_test_pump() { pump(nullptr); }
void monitor_test_time(double now) { test_clock=now; }
#endif
}

bool obs_module_load(void)
{
  stopping=false;
  obs_source_info info={}; info.id=ID;info.type=OBS_SOURCE_TYPE_FILTER;
  info.output_flags=OBS_SOURCE_VIDEO;info.get_name=name;info.create=create;info.destroy=destroy;
  info.get_defaults=defaults;info.get_properties=get_properties;info.video_render=render;
  info.filter_add=filter_added;info.load=load;info.update=update;
  obs_register_source(&info);obs_add_tick_callback(tick,nullptr);
#ifdef MODULATOR_DOCK
  blog(LOG_INFO,"[obs-modulator] Loaded %s (LFO + monitor)", MODULATOR_VERSION);
#else
  blog(LOG_INFO,"[obs-modulator] Loaded %s (LFO only)", MODULATOR_VERSION);
#endif
  return true;
}
void obs_module_post_load(void)
{
#ifdef MODULATOR_DOCK
  modulator::install_frontend();
#endif
}
void obs_module_unload(void)
{
#ifdef MODULATOR_DOCK
  modulator::shutdown_frontend();
#endif
  stopping=true;obs_remove_tick_callback(tick,nullptr);
  // Frontend/module shutdown must drain its UI queue before unloading code.
  obs_queue_task(OBS_TASK_UI,pump,nullptr,true);
}
