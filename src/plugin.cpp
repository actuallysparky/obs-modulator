// SPDX-FileCopyrightText: 2026 Sparky and OBS Modulator contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include <obs-module.h>
#include <util/platform.h>
#include "lfo.hpp"
#include "midi-clock.hpp"
#include "midi-input.hpp"
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
#include <optional>
#include <tuple>
#include <set>
#include <string>
#include <vector>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-modulator", "en-US")
MODULE_EXPORT const char *obs_module_description(void)
{ return "LFO automation of numeric source and filter settings"; }
MODULE_EXPORT const char *obs_module_author(void) { return "Sparky"; }

namespace {
namespace midi = modulator::midi;
constexpr const char *ID = "sparky_lfo_modulator";
std::string midi_note_label(int number)
{
  constexpr const char *names[] = {"C","C♯","D","D♯","E","F","F♯","G","G♯","A","A♯","B"};
  return names[number%12]+std::to_string(number/12-2)+" ("+std::to_string(number)+")";
}
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
// Controller parameters have session-only values. Settings remain the operator's
// baseline, so saving a scene during feedback never saves a random phase value.
struct AnimatedValue {
  double baseline;
  double value;
};
std::mutex parameter_mutex;
std::map<std::string, std::map<std::string, AnimatedValue>> animated_parameters;

bool controller_number(const std::string &key, Numeric &number, obs_source_t *target = nullptr)
{
  if (target && key == "period" && obs_data_get_int(settings(target).get(), "midi_mode") != 0) return false;
  if (key == "period") number = {key, "Cycle time (seconds)", false, .05, 86400, .05};
  else if (key == "minimum") number = {key, "Minimum", false, -1e12, 1e12, .01};
  else if (key == "maximum") number = {key, "Maximum", false, -1e12, 1e12, .01};
  else return false;
  return true;
}

double animated_value(const std::string &id, const std::string &key, double baseline)
{
  std::lock_guard<std::mutex> lock(parameter_mutex);
  auto controller = animated_parameters.find(id);
  if (controller == animated_parameters.end()) return baseline;
  auto &parameters = controller->second;
  auto parameter = parameters.find(key);
  if (parameter == parameters.end()) return baseline;
  if (parameter->second.baseline == baseline) return parameter->second.value;
  parameters.erase(parameter);
  if (parameters.empty()) animated_parameters.erase(controller);
  return baseline;
}

void write_parameter(obs_source_t *target, const Numeric &number, double baseline, double value)
{
  std::lock_guard<std::mutex> lock(parameter_mutex);
  animated_parameters[obs_source_get_uuid(target)].insert_or_assign(number.key, AnimatedValue{baseline, value});
}

double read_base_value(obs_source_t *target, const Numeric &n)
{
  auto d = settings(target);
  return n.integer ? static_cast<double>(obs_data_get_int(d.get(), n.key.c_str()))
                   : obs_data_get_double(d.get(), n.key.c_str());
}
double read_value(obs_source_t *target, const Numeric &n)
{
  const double baseline = read_base_value(target, n);
  return is_controller(target) ? animated_value(obs_source_get_uuid(target), n.key, baseline) : baseline;
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
  const bool nested;
  const double baseline;
  std::atomic<double> expected{0};
  std::atomic<uint64_t> write_generation{0};
  std::atomic<bool> wrote{false}, external{false}, dirty{false};
  std::shared_ptr<const Scalars> dependents=std::make_shared<const Scalars>();
  bool matches(obs_source_t *source) const {
    if (nested) {
      const double saved = read_base_value(source, number);
      return saved == baseline &&
             modulator::same_value(animated_value(obs_source_get_uuid(source), number.key, saved),
                                   expected, false);
    }
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
      property_generation(context ? context->generation : 0),
      nested(is_controller(source)), baseline(nested ? read_base_value(source, number) : 0)
  {
    auto *signals = obs_source_get_signal_handler(source);
    signal_handler_connect(signals, "update", changed, this);
    if (!nested) signal_handler_connect(signals, "update_properties", schema, this);
  }
  ~Watch()
  {
    auto source = own(obs_weak_source_get_source(target));
    if (source) {
      auto *signals = obs_source_get_signal_handler(source.get());
      signal_handler_disconnect(signals, "update", changed, this);
      if (!nested) signal_handler_disconnect(signals, "update_properties", schema, this);
    }
    obs_weak_source_release(target);
  }
};

enum class MidiMode { Seconds, Clock, CC };
struct MidiConfig {
  MidiMode mode;
  std::string clock, note, cc;
  int note_channel, note_number, cc_channel, cc_number, meter_top, meter_bottom;
  midi::Division division;
  bool aligned, hold, pickup;
  auto fields() const {
    return std::tie(mode, clock, note, cc, note_channel, note_number, cc_channel, cc_number,
                    meter_top, meter_bottom, division, aligned, hold, pickup);
  }
  bool operator==(const MidiConfig &other) const { return fields()==other.fields(); }
};
MidiConfig midi_config(obs_data_t *d)
{
  const auto integer = [d](const char *key, int low, int high) {
    return static_cast<int>(std::clamp(obs_data_get_int(d,key),
                                      static_cast<long long>(low),static_cast<long long>(high)));
  };
  const int denominator=integer("midi_meter_bottom",1,32);
  return {static_cast<MidiMode>(integer("midi_mode",0,2)),
    obs_data_get_string(d,"midi_clock_port"), obs_data_get_string(d,"midi_note_port"),
    obs_data_get_string(d,"midi_cc_port"), integer("midi_note_channel",1,16),
    integer("midi_note",0,127), integer("midi_cc_channel",1,16), integer("midi_cc",0,127),
    integer("midi_meter_top",1,32), (denominator & (denominator-1)) ? 4 : denominator,
    static_cast<midi::Division>(integer("midi_division",0,5)), obs_data_get_bool(d,"midi_align"),
    obs_data_get_bool(d,"midi_hold"), !obs_data_has_user_value(d,"midi_pickup") || obs_data_get_bool(d,"midi_pickup")};
}
struct Config {
  std::string source, filter, key;
  int type = 0, wave = 0;
  double low = 0, high = 0, period = 4;
  bool ease_in = false, ease_out = false, one_shot = false;
  MidiConfig midi{};
  bool operator==(const Config &b) const {
    return source==b.source && filter==b.filter && key==b.key && type==b.type &&
      wave==b.wave && low==b.low && high==b.high && period==b.period && ease_in==b.ease_in && ease_out==b.ease_out && one_shot==b.one_shot && midi==b.midi;
  }
};
Config config(obs_data_t *d)
{
  return {obs_data_get_string(d,"source_uuid"), obs_data_get_string(d,"filter_uuid"),
    obs_data_get_string(d,"property"), static_cast<int>(obs_data_get_int(d,"property_type")),
    static_cast<int>(obs_data_get_int(d,"waveform")), obs_data_get_double(d,"minimum"),
    obs_data_get_double(d,"maximum"), obs_data_get_double(d,"period"), obs_data_get_bool(d,"ease_in"),
    obs_data_has_user_value(d,"ease_out") ? obs_data_get_bool(d,"ease_out") : obs_data_get_bool(d,"ease_in"),
    obs_data_get_bool(d,"one_shot") && obs_data_get_int(d,"midi_mode")!=2,midi_config(d)};
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
  double play_value = 0, play_baseline = 0;
  uint64_t play_revision = 0;
  std::string play_uuid, play_key;
  double phase_anchor = 0, anchor_time = 0, applied_period = 4;
  double checked = 0, snapshot = 0, ease_start = 0;
  double release_started = 0, release_period = 4, release_from = 0;
  std::optional<double> cue, note_position;
  uint64_t clock_generation=0, clock_stop_generation=0, clock_loss_generation=0;
  bool clock_bound=false, clock_reanchor=false, release_seconds=false, note_held=false, cc_caught=false, cc_claim=false;
  bool cc_reconnecting=false;
  double last_clock_period=0, musical_time=0;
  std::optional<int> cc_previous;
  std::optional<double> pickup_previous;
  std::vector<int> cc_values;
  int learn=0;
  double learn_from=0;
  std::string midi_event;

  double phase(double now) const { return phase_anchor + (now - anchor_time) / applied_period; }
  void start_cycle(double now, double period) {
    phase_anchor = 0;
    anchor_time = now;
    applied_period = period;
  }
  void change_period(double now, double period) {
    if (period == applied_period) return;
    phase_anchor = phase(now);
    anchor_time = now;
    applied_period = period;
  }
  uint64_t glider_seed = UINT64_C(14695981039346656037);
  std::string snapshot_uuid, snapshot_key, status = "Disabled — choose a target, then enable the eye";
  std::string identity, ui_source, ui_filter, ui_key;
  bool snapshot_integer = false, feedback_loop = false;
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
midi::Input midi_input;
std::map<std::string,midi::Clock> midi_clocks;
#ifdef MODULATOR_TESTING
std::vector<midi::Event> injected_midi;
#endif
std::set<std::string> selected_ports;
double checked_ports=-1;

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
  if (found && obs_source_removed(found.get())) return own(nullptr);
  return found;
}
bool find_numeric(obs_source_t *target, const std::string &key, Numeric &n)
{
  if (!target) return false;
  if (is_controller(target)) return controller_number(key, n, target);
  auto p = properties(target);return find_numeric(p.get(),key,n);
}
bool find_numeric(obs_properties_t *props, const std::string &key, Numeric &n)
{
  std::vector<Numeric> all;numeric_properties(props, all);
  auto it = std::find_if(all.begin(), all.end(), [&](const Numeric &v) {return v.key == key;});
  if (it == all.end()) return false;
  n = *it; return true;
}
struct FeedbackLoop {
  std::string signature;
  bool accepted = false;
};
using FeedbackLoops = std::map<std::string, std::shared_ptr<const FeedbackLoop>>;
struct FeedbackEdge { std::string destination, key, consent; };
using FeedbackEdges = std::map<std::string, FeedbackEdge>;

void collect_feedback_edge(FeedbackEdges &edges, obs_source_t *self, const Config &cfg,
                           obs_data_t *data)
{
  Numeric number;
  if (cfg.filter.empty() || cfg.type != 2 || !controller_number(cfg.key, number)) return;
  auto target = resolve(self, cfg);
  if (target && is_controller(target.get()) && controller_number(cfg.key,number,target.get()) && cfg.midi.mode!=MidiMode::CC)
    edges.emplace(obs_source_get_uuid(self), FeedbackEdge{
      obs_source_get_uuid(target.get()), cfg.key, obs_data_get_string(data, "feedback_consent")});
}

FeedbackLoops feedback_loops(const FeedbackEdges &edges)
{
  FeedbackLoops result;
  std::set<std::string> visited;
  for (const auto &entry : edges) {
    std::set<std::string> path;
    std::string id = entry.first;
    while (edges.count(id) && !visited.count(id) && path.insert(id).second)
      id = edges.at(id).destination;
    visited.insert(path.begin(), path.end());
    if (!path.count(id)) continue;

    std::set<std::string> members;
    for (std::string next = id; members.insert(next).second; next = edges.at(next).destination) {}
    auto loop = std::make_shared<FeedbackLoop>();
    for (const auto &member : members) {
      const auto &edge = edges.at(member);
      loop->signature += member + "/" + edge.destination + "/" + edge.key + ";";
    }
    for (const auto &member : members)
      if (edges.at(member).consent == loop->signature) loop->accepted = true;
    for (const auto &member : members) result.emplace(member, loop);
  }
  return result;
}

FeedbackLoop selected_feedback(obs_source_t *self, obs_data_t *draft)
{
  std::vector<Handle> list;
  { std::lock_guard<std::mutex> lock(registry_mutex); list = controllers; }
  FeedbackEdges edges;
  for (const auto &handle : list) {
    if (handle->dead) continue;
    auto source = own(obs_weak_source_get_source(handle->self));
    if (!source || obs_source_removed(source.get())) continue;
    auto saved = settings(source.get());
    auto *data = source.get() == self ? draft : saved.get();
    collect_feedback_edge(edges, source.get(), config(data), data);
  }
  const auto loops = feedback_loops(edges);
  const auto it = loops.find(obs_source_get_uuid(self));
  return it == loops.end() ? FeedbackLoop{} : *it->second;
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
  c.restart=false; c.play=false; c.play_value_valid=false; c.armed=false;
  c.cue.reset();c.note_position.reset();c.cc_claim=false;c.cc_caught=false;c.pickup_previous.reset();c.cc_values.clear();++c.revision;
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
  return midi::Input::now();
}
void finish(Controller &c, obs_source_t *self, const Config &cfg)
{
  if(cfg.one_shot) {
    if(c.cue) {
      c.active=false;c.releasing=false;c.natural_return=false;
      status(c,self,"Queued — next downbeat");
      return;
    }
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
bool request_play(Controller &c, obs_source_t *self);

std::vector<midi::Event> midi_events(const std::set<std::string> &ports, double now)
{
  if(ports!=selected_ports || (!ports.empty() && (checked_ports<0 || now-checked_ports>=1))) {
    midi_input.select(ports);
    selected_ports=ports;checked_ports=now;
  }
  auto events=midi_input.drain();
#ifdef MODULATOR_TESTING
  events.insert(events.end(),injected_midi.begin(),injected_midi.end());
  injected_midi.clear();
#endif
  std::stable_sort(events.begin(),events.end(),[](const auto &a,const auto &b) {
    const bool a_gone=a.kind==midi::Kind::Disconnected,b_gone=b.kind==midi::Kind::Disconnected;
    return a_gone!=b_gone ? a_gone : a.time<b.time;
  });
  for(auto i=midi_clocks.begin();i!=midi_clocks.end();)
    if(!ports.count(i->first))i=midi_clocks.erase(i);else ++i;
  return events;
}

midi::ClockSample clock_for(const MidiConfig &cfg, double now)
{
  auto clock=midi_clocks.find(cfg.clock);
  return clock==midi_clocks.end() ? midi::ClockSample{} : clock->second.sample(now);
}

void feed_clock(const midi::Event &event)
{
  auto &clock=midi_clocks[event.port];
  switch(event.kind) {
    case midi::Kind::Clock:clock.receive(midi::ClockMessage::Tick,event.time);break;
    case midi::Kind::Start:clock.receive(midi::ClockMessage::Start,event.time);break;
    case midi::Kind::Continue:clock.receive(midi::ClockMessage::Continue,event.time);break;
    case midi::Kind::Stop:clock.receive(midi::ClockMessage::Stop,event.time);break;
    case midi::Kind::Disconnected:clock.disconnect();break;
    default:break;
  }
}

void apply_midi_events(Controller &c, obs_source_t *self, const Config &cfg,
                       const std::vector<midi::Event> &events,
                       const std::vector<std::map<std::string,double>> &note_positions)
{
  const auto &binding=cfg.midi;
  auto *parent=obs_filter_get_parent(self);
  const bool showing=parent && obs_source_showing(parent);
  const int learning=c.learn;
  for(size_t i=0;i<events.size();++i) {
    const auto &event=events[i];
    const bool note=event.kind==midi::Kind::NoteOn && event.value>0;
    const bool release=event.kind==midi::Kind::NoteOff ||
                       (event.kind==midi::Kind::NoteOn && event.value==0);
    const bool cc=event.kind==midi::Kind::ControlChange && event.number<120;
    if(event.kind==midi::Kind::Disconnected) {
      if(!binding.note.empty() && event.port==binding.note && c.note_held) {
        c.note_held=false;
        if(binding.hold && !cfg.one_shot)obs_source_set_enabled(self,false);
      }
      if(!binding.cc.empty() && event.port==binding.cc) {
        c.cc_caught=false;c.pickup_previous.reset();c.cc_claim=false;c.cc_values.clear();c.cc_previous.reset();
        c.cc_reconnecting=true;c.watch.reset();
      }
      continue;
    }
    if(learning && event.time>=c.learn_from && (learning==1 ? note : cc)) {
      const bool learning_note=c.learn==1;
      c.learn=0;
      auto d=settings(self);
      obs_data_set_string(d.get(),learning_note ? "midi_note_port" : "midi_cc_port",event.port.c_str());
      obs_data_set_int(d.get(),learning_note ? "midi_note_channel" : "midi_cc_channel",event.channel);
      obs_data_set_int(d.get(),learning_note ? "midi_note" : "midi_cc",event.number);
      obs_source_update(self,d.get());
      status(c,self,"MIDI binding learned");
      return;
    }
    if(learning)continue;
    if(binding.mode==MidiMode::CC) {
      if(cc && event.port==binding.cc && event.channel==binding.cc_channel && event.number==binding.cc_number) {
        if(showing && obs_source_enabled(self) && !c.cc_reconnecting && c.cc_previous!=event.value)
          c.cc_values.push_back(event.value);
        c.cc_reconnecting=false;
        c.cc_previous=event.value;
        c.midi_event="CC "+std::to_string(event.number)+" = "+std::to_string(event.value);
      }
      continue;
    }
    if((!note && !release) || binding.note.empty() || event.port!=binding.note ||
       event.channel!=binding.note_channel || event.number!=binding.note_number)continue;
    c.midi_event=midi_note_label(event.number)+(note ? " on" : " off");
    if(release) {
      if(c.note_held && binding.hold && !cfg.one_shot)obs_source_set_enabled(self,false);
      c.note_held=false;
      continue;
    }
    if(binding.hold && !cfg.one_shot && c.note_held)continue;
    c.note_held=true;
    const auto position=note_positions[i].find(binding.clock);
    c.note_position=position==note_positions[i].end() ? std::nullopt : std::optional<double>(position->second);
    if(cfg.one_shot) {
      request_play(c,self);
    } else {
      const bool enable=binding.hold || !obs_source_enabled(self);
      obs_source_set_enabled(self,enable);
      if(!enable)c.note_position.reset();
    }
  }
}

struct MidiTakeover { Controller *control; std::string target, key; };
void cancel_lfo_for_cc(const std::vector<Handle> &list, const MidiTakeover &claim)
{
  for(const auto &h:list) {
    std::lock_guard<std::recursive_mutex> lock(h->mutex);
    auto self=own(obs_weak_source_get_source(h->self));
    if(!self || h.get()==claim.control || h->dead)continue;
    const auto cfg=config(settings(self.get()).get());
    if(cfg.midi.mode==MidiMode::CC)continue;
    const bool dependent=h->watch && std::atomic_load(&h->watch->dependents)->count(claim.key);
    if(cfg.key!=claim.key && !dependent)continue;
    auto target=resolve(self.get(),cfg);
    if(!target || claim.target!=obs_source_get_uuid(target.get()))continue;
    if(obs_source_enabled(self.get()) || h->active || h->cue)
      disable(*h,self.get(),"Paused — MIDI control took over",Controller::StopReason::ExternalEdit);
  }
}

bool prepare_clock_target(Controller &c, obs_source_t *self, const Config &cfg, double now)
{
  if(c.watch)return true;
  auto target=resolve(self,cfg);
  if(!target)return false;
  const bool nested=is_controller(target.get());
  auto context=nested ? nullptr : property_context(target.get(),now);
  Numeric number;
  const bool found=nested ? controller_number(cfg.key,number,target.get())
                          : find_numeric(context->props.get(),cfg.key,number);
  if(!found || cfg.type!=(number.integer?1:2) || cfg.low<number.low || cfg.high>number.high)return false;
  c.watch=std::make_shared<Watch>(target.get(),number,context);
  c.watch->expected=read_value(target.get(),number);
  c.watch->wrote=true;
  return true;
}

void pump(void *)
{
  // One frame-paced UI task, partial writes batched by target. No shot timers.
  if (stopping) {
    midi_input.select({});
    midi_clocks.clear();selected_ports.clear();checked_ports=-1;
    property_contexts.clear();
    std::lock_guard<std::mutex> lock(parameter_mutex);
    animated_parameters.clear();
    pending = false;
    return;
  }
  for(auto i=property_contexts.begin();i!=property_contexts.end();)
    if(i->second.expired())i=property_contexts.erase(i);else ++i;
  std::vector<Handle> list;
  {std::lock_guard<std::mutex> lock(registry_mutex);list=controllers;}
  // Resolve configuration once per controller, not once per sort comparison.
  std::set<std::string> inputs, clocks;
  for(const auto &h:list) {
    std::lock_guard<std::recursive_mutex> lock(h->mutex);
    auto self=own(obs_weak_source_get_source(h->self));
    if(!self || h->dead)continue;
    const auto cfg=midi_config(settings(self.get()).get());
    if(cfg.mode==MidiMode::Clock && !cfg.clock.empty())clocks.insert(cfg.clock);
    if(cfg.mode==MidiMode::CC && !cfg.cc.empty())inputs.insert(cfg.cc);
    if(cfg.mode!=MidiMode::CC && !cfg.note.empty())inputs.insert(cfg.note);
    if(!h->learn)continue;
    for(const auto &port:midi_input.ports()) inputs.insert(port.id);
  }
  inputs.insert(clocks.begin(),clocks.end());
  const double midi_now=engine_now();
  const auto events=midi_events(inputs,midi_now);
  std::vector<std::map<std::string,double>> note_positions(events.size());
  for(size_t i=0;i<events.size();++i) {
    const auto &event=events[i];
    if(clocks.count(event.port))feed_clock(event);
    if(event.kind==midi::Kind::NoteOn && event.value>0) {
      for(const auto &id:clocks) {
        const auto it=midi_clocks.find(id);
        if(it!=midi_clocks.end()) {
          const auto sample=it->second.sample(event.time);
          if(sample.valid && !sample.stopped)note_positions[i][id]=sample.position;
        }
      }
    }
  }
  for(auto &clock:midi_clocks)clock.second.update(midi_now);
  std::vector<MidiTakeover> claims;
  if(!events.empty())for(auto &h:list) {
    std::lock_guard<std::recursive_mutex> lock(h->mutex);
    auto self=own(obs_weak_source_get_source(h->self));
    if(!self || h->dead)continue;
    const auto cfg=config(settings(self.get()).get());
    if(!(cfg.midi==h->last.midi)) {
      h->cc_caught=false;h->pickup_previous.reset();h->cc_claim=false;h->cc_previous.reset();h->cc_values.clear();
      h->cc_reconnecting=false;h->note_held=false;h->midi_event.clear();
      if(h->last.midi.hold && h->last.midi.mode!=MidiMode::CC && !h->last.midi.note.empty())
        obs_source_set_enabled(self.get(),false);
    }
    apply_midi_events(*h,self.get(),cfg,events,note_positions);
    if(!h->cc_values.empty()) {
      auto target=resolve(self.get(),cfg);Numeric number;
      if(target && find_numeric(target.get(),cfg.key,number) && cfg.type==(number.integer?1:2)
         && cfg.low>=number.low && cfg.high<=number.high && cfg.low<=cfg.high)
        claims.push_back({h.get(),obs_source_get_uuid(target.get()),cfg.key});
    }
  }
  for(const auto &claim:claims)cancel_lfo_for_cc(list,claim);
  std::map<Controller *,bool> established;
  FeedbackEdges edges;
  for (auto &h : list) {
    std::lock_guard<std::recursive_mutex> lock(h->mutex);
    auto self = own(obs_weak_source_get_source(h->self));
    established[h.get()] = false;
    if (!self || h->dead || obs_source_removed(self.get())) continue;
    auto data = settings(self.get());
    const auto cfg = config(data.get());
    established[h.get()] = (h->active || h->cc_claim) && cfg == h->last;
    collect_feedback_edge(edges, self.get(), cfg, data.get());
  }
  std::set<Controller *> direct_requests;
  for(const auto &claim:claims)direct_requests.insert(claim.control);
  std::sort(list.begin(),list.end(),[&](const Handle &a,const Handle &b){
    const bool direct_a=direct_requests.count(a.get())!=0, direct_b=direct_requests.count(b.get())!=0;
    if(direct_a!=direct_b)return direct_a;
    const bool aa=established.at(a.get()),bb=established.at(b.get());
    return aa!=bb ? aa>bb : a->identity<b->identity;
  });
  std::map<std::string,Controller *> owners;
  std::map<std::string,Batch> batches;
  const auto loops = feedback_loops(edges);
  struct ParameterRestore { Source target; Numeric number; double baseline, value; };
  std::vector<ParameterRestore> parameter_restores;
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
    const bool direct=cfg.midi.mode==MidiMode::CC;
    const bool synced=cfg.midi.mode==MidiMode::Clock;
    auto cc_values=std::move(c.cc_values);c.cc_values.clear();
    const auto loop = loops.find(c.identity);
    c.feedback_loop = loop != loops.end();
    if(c.restore.exchange(false)) {
      disable(c,self.get(),"Disabled — restored starting value",Controller::StopReason::None);
      auto target=resolve(self.get(),cfg);Numeric n;
      if(c.snapshot_valid && target && c.snapshot_uuid==obs_source_get_uuid(target.get()) &&
         c.snapshot_key==cfg.key && find_numeric(target.get(),cfg.key,n) && n.integer==c.snapshot_integer) {
        const double restored = std::clamp(c.snapshot, n.low, n.high);
        if (is_controller(target.get())) {
          const double baseline = read_base_value(target.get(), n);
          parameter_restores.push_back({std::move(target), n, baseline, restored});
        } else {
          auto context=property_context(target.get(),now);auto current=settings(target.get());
          write(current,n,restored);
          if(obs_property_modified(obs_properties_get(context->props.get(),n.key.c_str()),current.get()))context->dirty=true;
          if(!obs_source_removed(target.get()))obs_source_update(target.get(),nullptr);
        }
      } else {c.stop_reason=Controller::StopReason::Invalid;status(c,self.get(),"Disabled — starting value unavailable for this target");}
      continue;
    }
    if(!(cfg==c.last)) {
      if(events.empty() && !(cfg.midi==c.last.midi)) {
        c.cc_previous.reset();c.cc_reconnecting=false;c.note_held=false;c.midi_event.clear();
        if(c.last.midi.hold && !c.last.midi.note.empty())obs_source_set_enabled(self.get(),false);
      }
      const bool cancel=(cfg.one_shot || c.last.one_shot) &&
        (c.active || (obs_source_enabled(self.get()) && !gate_changed && !c.play));
      c.active=false;c.releasing=false;c.natural_return=false;c.watch.reset();c.last=cfg;++c.revision;
      c.cue.reset();c.clock_bound=false;c.clock_reanchor=false;c.last_clock_period=0;c.cc_claim=false;c.cc_caught=false;c.pickup_previous.reset();
      if(cancel){disable(c,self.get(),"Cancelled — configuration changed; Play to run again",Controller::StopReason::Cancelled);continue;}
    }
    auto *parent=obs_filter_get_parent(self.get());
    bool enabled=obs_source_enabled(self.get());
    if (c.feedback_loop && !loop->second->accepted && (enabled || c.active)) {
      disable(c,self.get(),"Disabled — feedback loop needs acknowledgement in filter properties");
      continue;
    }
    bool requested=enabled && parent && obs_source_showing(parent);
    if(cfg.one_shot && gate_changed && enabled && !c.active){c.armed=true;c.stop_reason=Controller::StopReason::None;}
    // Check the old watch BEFORE processing a queued Play/restart/eye edge.
    auto watched=own(c.watch ? obs_weak_source_get_source(c.watch->target):nullptr);
    if(c.watch && watched && (c.watch->external || (c.watch->wrote && !c.watch->matches(watched.get())))) {
      if(direct) {
        c.cc_claim=false;c.cc_caught=false;c.pickup_previous.reset();c.watch.reset();
        status(c,self.get(),"Waiting — manual edit; move fader to pick up");continue;
      }
      disable(c,self.get(),"Paused — target changed externally; enable the eye to restart",Controller::StopReason::ExternalEdit);continue;
    }
    if(direct && (!requested || (cc_values.empty() && !c.cc_claim))) {
      c.cc_claim=false;c.cc_caught=false;c.pickup_previous.reset();c.watch.reset();
      if(requested)status(c,self.get(),cfg.midi.cc.empty() ? "Waiting — choose a MIDI CC input" : "Waiting — move mapped fader");
      else status(c,self.get(),enabled ? "Waiting — parent source is hidden" : "Disabled — enable the eye for MIDI CC");
      continue;
    }
    const bool play=c.play.exchange(false);
    bool retrigger=play || (cfg.one_shot && gate_changed && enabled && c.active);
    if(gate_changed && enabled && !cfg.one_shot) {c.active=false;c.releasing=false;c.watch.reset();}
    if(cfg.one_shot && c.active && !requested && enabled) {
      // An interrupted shot cannot replay merely because the parent reappears.
      obs_source_set_enabled(self.get(),false);c.gate_changed=false;enabled=false;
    }
    if(!synced && retrigger && c.active) {c.releasing=false;c.natural_return=false;}
    const auto clock=synced ? clock_for(cfg.midi,now) : midi::ClockSample{};
    const double division=synced ? midi::division_quarters(cfg.midi.division,cfg.midi.meter_top,cfg.midi.meter_bottom) : 1;
    std::optional<double> musical_start;
    if(synced) {
      if(c.clock_bound && c.clock_loss_generation!=clock.loss_generation &&
         (requested || c.natural_return) && (c.active || c.cue)) {
        disable(c,self.get(),"Clock lost — trigger again when the clock returns",Controller::StopReason::Cancelled);
        continue;
      }
      const bool new_reference=c.clock_bound && c.clock_generation!=clock.generation;
      if(new_reference){c.cue.reset();c.clock_reanchor=c.active;}
      if(c.clock_bound && c.clock_stop_generation!=clock.stop_generation) {
        c.cue.reset();
        if(!c.active && enabled) {
          disable(c,self.get(),"MIDI stopped — queued start cancelled",Controller::StopReason::Cancelled);
          continue;
        }
      }
      c.clock_bound=true;c.clock_generation=clock.generation;
      c.clock_loss_generation=clock.loss_generation;c.clock_stop_generation=clock.stop_generation;
      c.musical_time=clock.position;
      if(clock.valid)c.last_clock_period=clock.seconds_per_quarter*division;
      if(!requested) {
        c.cue.reset();c.note_position.reset();
      } else {
        if(clock.stopped || !clock.valid || (cfg.midi.aligned && !clock.aligned)) {
          if(play && !c.active)c.armed=true;
          status(c,self.get(),clock.stopped ? "MIDI stopped — holding phase" : clock.lost ?
            "Clock lost — trigger again when the clock returns" : cfg.midi.aligned && clock.valid ?
            "Clock only — waiting for MIDI Start" : "Waiting — MIDI clock");
          continue;
        }
        const bool restart=c.restart.exchange(false);
        const bool start=!c.active || retrigger || restart || new_reference;
        if(c.clock_reanchor) {
          musical_start=0;
          retrigger=c.active;
          c.clock_reanchor=false;
        } else if(cfg.midi.aligned) {
          if(start && !c.cue) {
            const double at_note=c.note_position.value_or(clock.position);
            c.cue=midi::next_downbeat(new_reference ? clock.position : at_note,
                                    cfg.midi.meter_top,cfg.midi.meter_bottom);
          }
          if(c.cue && clock.confirmed_position>=*c.cue) {
            musical_start=*c.cue;c.cue.reset();retrigger=c.active;
          } else {
            retrigger=false;
            if(!c.active) {
              if(!prepare_clock_target(c,self.get(),cfg,now)) {
                disable(c,self.get(),"Disabled — target unavailable for queued MIDI start");
                continue;
              }
              status(c,self.get(),"Queued — next downbeat");
              continue;
            }
          }
        } else if(start) {
          musical_start=new_reference ? clock.confirmed_position : c.note_position.value_or(clock.position);
          retrigger=c.active;
        }
        c.note_position.reset();
      }
    }
    if(!direct && !requested && (!parent || !c.active || !cfg.ease_out || !c.snapshot_valid)) {
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
    double low = animated_value(c.identity, "minimum", cfg.low);
    double high = animated_value(c.identity, "maximum", cfg.high);
    const double period = animated_value(c.identity, "period", cfg.period);
    const bool animated_range = low != cfg.low || high != cfg.high;
    if (animated_range && low > high) std::swap(low, high);
    if(!std::isfinite(cfg.low) || !std::isfinite(cfg.high) || cfg.low>cfg.high ||
       !std::isfinite(cfg.period) || cfg.period<0.05 || cfg.period>86400 || cfg.wave<0 || cfg.wave>=modulator::wave_count) {
      disable(c,self.get(),"Disabled — invalid range, waveform, or cycle time");continue;
    }
    auto target=resolve(self.get(),cfg);
    if(!target){disable(c,self.get(),"Disabled — target was removed; select it again");continue;}
    const std::string uuid=obs_source_get_uuid(target.get());
    if(c.active && c.snapshot_uuid!=uuid){disable(c,self.get(),"Disabled — target identity changed; enable the eye to restart");continue;}
    const std::string ownership=uuid+"/"+cfg.key;
    if(direct && cc_values.empty())continue;
    if(owners.count(ownership)){
      if(direct) {
        c.cc_claim=false;c.cc_caught=false;c.pickup_previous.reset();c.watch.reset();
        status(c,self.get(),"Waiting — another mapping owns this property");
      } else disable(c,self.get(),owners.at(ownership)->last.midi.mode==MidiMode::CC ?
        "Disabled — MIDI control owns this property" : "Disabled — another LFO owns this property");
      continue;
    }
    owners[ownership]=&c;
    if(c.watch)for(const auto &dependent:*std::atomic_load(&c.watch->dependents))
      owners.emplace(uuid+"/"+dependent.first,&c);
    const bool nested = is_controller(target.get());
    auto context = nested ? nullptr : property_context(target.get(), now);
    const auto generation = context ? context->generation : 0;
    if(!c.watch || now-c.checked>=1 || c.watch->dirty.exchange(false) || c.watch->property_generation!=generation) {
      Numeric n;c.checked=now;
      const bool found = nested ? controller_number(cfg.key, n,target.get()) : find_numeric(context->props.get(), cfg.key, n);
      if(!found || cfg.type!=(n.integer?1:2)) {
        disable(c,self.get(),"Disabled — property missing, hidden, disabled, or changed type");continue;
      }
      if(cfg.low<n.low || cfg.high>n.high){disable(c,self.get(),"Disabled — modulation range exceeds target limits");continue;}
      if(!c.watch)c.watch=std::make_shared<Watch>(target.get(),n,context);
      c.watch->property_generation=generation;
      c.watch->number.low=n.low;c.watch->number.high=n.high;
    }
    auto &w=*c.watch;
    low = std::clamp(low, w.number.low, w.number.high);
    high = std::clamp(high, w.number.low, w.number.high);
    if(requested && c.releasing && !c.natural_return && !cfg.one_shot){c.active=false;c.releasing=false;}
    const double current=read_value(target.get(),w.number);
    if(direct) {
      c.cc_claim=true;
      const auto scaled=[&](int value) {return low+(high-low)*value/127.0;};
      double value=current;
      for(int input:cc_values) {
        const double next=scaled(input);
        if(!cfg.midi.pickup || modulator::same_value(next,current,w.number.integer))c.cc_caught=true;
        if(c.cc_caught)value=next;
        if(!c.cc_caught) {
          // Use a separate physical sample for the next pickup crossing.
          if(c.pickup_previous && (next-current)*(*c.pickup_previous-current)<=0) {
            c.cc_caught=true;value=next;
          }
          c.pickup_previous=next;
        }
      }
      if(c.cc_caught) {
        value=std::clamp(w.number.integer ? std::round(value) : value,w.number.low,w.number.high);
        if(!modulator::same_value(current,value,w.number.integer)) {
          auto &batch=batches[uuid];if(!batch.target)batch.target=own(obs_source_get_ref(target.get()));
          batch.changes.push_back({h,w.number,current,value,c.revision.load(),false});
        }
      }
      if(!w.wrote){w.expected=current;w.wrote=true;}
      status(c,self.get(),c.cc_caught ? "MIDI CC — controlling target" : current<low || current>high ?
        "Waiting — current value is outside the fader range" : "Waiting — fader pickup");
      continue;
    }
    // Native property preparation can block. A fresh/retriggered performance
    // starts only once its target and starting value are ready, not at the old
    // frame timestamp sampled before that work. Existing runs still catch up
    // to elapsed time after stalls instead of extending their deadlines.
    const double wall_now=engine_now();
    const double motion_now=synced ? std::max(clock.position, musical_start.value_or(clock.position)) : wall_now;
    const double motion_period=synced ? division : period;
    if(c.play_value_valid) {
      const bool external=c.play_uuid==uuid && c.play_key==cfg.key &&
        (!modulator::same_value(current,c.play_value,w.number.integer) ||
         (nested && read_base_value(target.get(), w.number) != c.play_baseline));
      c.play_value_valid=false;
      if(external){disable(c,self.get(),"Paused — target changed after Play; operator value preserved",Controller::StopReason::ExternalEdit);continue;}
    }
    if(!c.active) {
      c.stop_reason=Controller::StopReason::None;c.armed=false;c.active=true;c.snapshot=current;c.ease_start=current;
      c.start_cycle(musical_start.value_or(motion_now), motion_period);
      c.snapshot_uuid=uuid;c.snapshot_key=cfg.key;c.snapshot_integer=w.number.integer;c.snapshot_valid=std::isfinite(current);
      c.releasing=false;c.natural_return=false;
      if(cfg.one_shot)++c.shot_index;
      retrigger=false;
    }
    if(requested && (c.restart.exchange(false) || retrigger)) {
      c.start_cycle(musical_start.value_or(motion_now), motion_period);
      c.ease_start=current;c.releasing=false;c.natural_return=false;
      if(cfg.one_shot)++c.shot_index;
    }
    const double phase = c.phase(motion_now);
    const double elapsed_period = c.applied_period;
    c.change_period(motion_now, motion_period);
    const auto wave=static_cast<modulator::Wave>(cfg.wave);
    auto shot=[&](double cycles){return modulator::shot_mapped(wave,cycles,1,low,high,
      w.number.integer,w.number.low,w.number.high,c.shot_seed,cfg.ease_in,c.ease_start,c.shot_index);};
    if(!requested) {
      c.restart=false;
      if(!c.releasing){
        c.releasing=true;c.release_seconds=synced && (clock.stopped || !clock.valid);
        c.release_started=c.release_seconds ? wall_now : motion_now;
        c.release_period=c.release_seconds ? c.last_clock_period : motion_period;
        c.release_from=current;
        if(c.release_period<=0) {finish(c,self.get(),cfg);continue;}
      } else if(synced && !c.release_seconds && (clock.stopped || !clock.valid)) {
        const double elapsed=std::clamp((motion_now-c.release_started)/c.release_period,0.0,1.0);
        c.release_seconds=true;c.release_period=c.last_clock_period;
        c.release_started=wall_now-elapsed*c.release_period;
      }
      c.natural_return=false;
    } else if(cfg.one_shot && !c.releasing && phase>=1 && cfg.ease_out) {
      c.releasing=true;c.natural_return=true;
      c.release_started=motion_now-(phase-1)*elapsed_period;
      c.release_period=motion_period;c.release_seconds=false;c.release_from=shot(1);
    }
    const double release_now=c.release_seconds ? wall_now : motion_now;
    const double value=c.releasing ? modulator::eased_return(release_now-c.release_started,c.release_period,c.release_from,c.snapshot,
        w.number.integer,w.number.low,w.number.high)
      : cfg.one_shot ? shot(phase)
      : modulator::mapped(wave,phase,1,low,high,w.number.integer,
          w.number.low,w.number.high,c.glider_seed,cfg.ease_in,c.ease_start);
    if(!std::isfinite(value)){disable(c,self.get(),"Disabled — non-finite target value");continue;}
    const bool ending=c.releasing ? release_now-c.release_started>=c.release_period : cfg.one_shot && phase>=1;
    if((ending && current!=value) || !modulator::same_value(current,value,w.number.integer)) {
      auto &batch=batches[uuid];if(!batch.target)batch.target=own(obs_source_get_ref(target.get()));
      batch.changes.push_back({h,w.number,current,value,c.revision.load(),ending});
    } else {
      w.expected=value;w.wrote=true;
      if(ending){finish(c,self.get(),cfg);continue;}
    }
    const uint64_t interval=obs_get_frame_interval_ns();
    std::string message = c.releasing ? "Easing out — returning to starting value; manual edits cancel"
      : interval && (synced ? c.last_clock_period : period)<8.0*static_cast<double>(interval)/1e9
      ? "Running — fewer than 8 frames per cycle; motion may alias" : "Running — manual target edits turn this eye off";
    if(synced && requested && c.cue)message="Queued — next downbeat";
    else if(synced && !c.releasing)message+=clock.aligned ? " · Clock and downbeat" : " · Clock only";
    if (c.feedback_loop) message += " · Feedback loop";
    status(c, self.get(), message);
  }
  // Every controller sees values from before this commit, including self-loops.
  for (auto &restore : parameter_restores) {
    if (!obs_source_removed(restore.target.get()) &&
        read_base_value(restore.target.get(), restore.number) == restore.baseline)
      write_parameter(restore.target.get(), restore.number, restore.baseline, restore.value);
  }
  for(auto &entry:batches) {
    auto &batch=entry.second;bool changed=false;
    if(obs_source_removed(batch.target.get()))continue;
    const bool nested = is_controller(batch.target.get());
    auto context = nested ? nullptr : property_context(batch.target.get(), now);
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
    for(const auto &h:list) {
      std::lock_guard<std::recursive_mutex> lock(h->mutex);
      if(h->last.midi.mode!=MidiMode::CC || !h->watch || direct_requests.count(h.get()))continue;
      auto watched_target=own(obs_weak_source_get_source(h->watch->target));
      if(watched_target.get()!=batch.target.get())continue;
      const auto owner=owners.find(entry.first+"/"+h->watch->number.key);
      if(owner==owners.end() || owner->second->last.midi.mode==MidiMode::CC)continue;
      // The operator started an LFO while the physical control was idle.
      // Retire the old CC watch so a later fader move can take over again.
      h->cc_claim=false;h->cc_caught=false;h->pickup_previous.reset();h->watch.reset();
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
      const bool found = nested ? controller_number(change.number.key, accepted,batch.target.get())
                                : find_numeric(context->props.get(),change.number.key,accepted);
      if(!found || accepted.integer!=change.number.integer ||
         change.value<accepted.low || change.value>accepted.high) {
        disable(c,self.get(),"Disabled — target property changed before its edit");continue;
      }
      auto watch=c.watch;watch->expected=change.value;watch->wrote=true;change.applied=true;
      if (nested) {
        write_parameter(batch.target.get(), accepted, watch->baseline, change.value);
        continue;
      }
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
          if(c.last.midi.mode==MidiMode::CC && owner->second->last.midi.mode!=MidiMode::CC) {
            auto &other=*owner->second;
            std::lock_guard<std::recursive_mutex> lock_other(other.mutex);
            auto other_self=own(obs_weak_source_get_source(other.self));
            if(other_self)disable(other,other_self.get(),"Paused — MIDI control took over",Controller::StopReason::ExternalEdit);
            owner->second=&c;
            dependencies->insert_or_assign(value.first,value.second);
            continue;
          }
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
void feedback_choices(obs_properties_t *props, const FeedbackLoop &loop)
{
  auto *notice = obs_properties_get(props, "feedback_notice");
  auto *allow = obs_properties_get(props, "allow_feedback");
  obs_property_set_visible(notice, !loop.signature.empty());
  obs_property_set_visible(allow, !loop.signature.empty() && !loop.accepted);
  obs_property_set_description(notice, loop.accepted ?
    "Feedback loop acknowledged. Enable its filters to run." :
    "This mapping is part of a feedback loop. The result may change rapidly or behave unpredictably. Acknowledge this loop to run it.");
}

bool feedback_changed(void *priv, obs_properties_t *props, obs_property_t *, obs_data_t *draft)
{
  if (!priv || !obs_data_get_bool(draft, "allow_feedback")) return false;
  // A transient checkbox action avoids replaying consent on property refresh or
  // on load. Save only the signature of the exact connections being reviewed.
  obs_data_set_bool(draft, "allow_feedback", false);
  auto self = own(obs_weak_source_get_source(static_cast<Controller *>(priv)->self));
  if (!self) return false;
  auto loop = selected_feedback(self.get(), draft);
  if (loop.signature.empty()) return true;
  obs_data_set_string(draft, "feedback_consent", loop.signature.c_str());
  loop.accepted = true;
  feedback_choices(props, loop);
  return true;
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
  feedback_choices(props, self ? selected_feedback(self.get(), d) : FeedbackLoop{});
  if (!self) {preserve();return;}
  Config cfg=config(d); cfg.filter.clear(); auto parent=resolve(self.get(),cfg);
  if (parent) obs_source_enum_filters(parent.get(),[](obs_source_t *,obs_source_t *f,void *p) {
    if (!obs_source_removed(f)) obs_property_list_add_string(static_cast<obs_property_t *>(p),obs_source_get_name(f),obs_source_get_uuid(f));
  },filters);
  auto target=resolve(self.get(),config(d));
  if (!target) {preserve();return;}
  std::vector<Numeric> all;
  if (is_controller(target.get())) {
    for (const char *key : {"period", "minimum", "maximum"}) {
      Numeric number;
      if (controller_number(key, number,target.get())) all.push_back(std::move(number));
    }
  } else {
    auto p=properties(target.get());
    numeric_properties(p.get(), all);
  }
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
  if(c.play_value_valid){
    c.play_value=read_value(target.get(),n);c.play_baseline=read_base_value(target.get(),n);
    c.play_uuid=obs_source_get_uuid(target.get());c.play_key=cfg.key;
  }
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
void port_choices(obs_properties_t *props, obs_data_t *d, const std::vector<midi::Port> &ports)
{
  for(const auto *key:{"midi_clock_port","midi_note_port","midi_cc_port"}) {
    auto *property=obs_properties_get(props,key);
    obs_property_list_clear(property);
    obs_property_list_add_string(property,"Choose an input","");
    for(const auto &port:ports)obs_property_list_add_string(property,port.name.c_str(),port.id.c_str());
    preserve_selection(property,obs_data_get_string(d,key),"Saved MIDI input is missing — select a replacement");
  }
}

bool midi_inputs_clicked(obs_properties_t *props, obs_property_t *, void *priv)
{
  if(!priv)return false;
  auto self=own(obs_weak_source_get_source(static_cast<Controller *>(priv)->self));
  if(!self)return false;
  auto d=settings(self.get());
  port_choices(props,d.get(),midi_input.ports());
  return true;
}

bool learn_clicked(obs_properties_t *, obs_property_t *property, void *priv)
{
  if(!priv)return false;
  auto &c=*static_cast<Controller *>(priv);
  std::lock_guard<std::recursive_mutex> lock(c.mutex);
  auto self=own(obs_weak_source_get_source(c.self));
  if(!self)return false;
  const std::string key=obs_property_name(property);
  c.learn=key=="learn_note" ? 1 : key=="learn_cc" ? 2 : 0;
  c.learn_from=engine_now();
  c.midi_event=c.learn ? "Learning — play a note or move a CC control" : "MIDI Learn cancelled";
  obs_source_update_properties(self.get());
  return false;
}

bool playback_choices(void *, obs_properties_t *props, obs_property_t *, obs_data_t *d)
{
  const Config cfg=config(d);
  const bool cc=cfg.midi.mode==MidiMode::CC;
  const bool clock=cfg.midi.mode==MidiMode::Clock;
  const bool note_selected=!cc && !cfg.midi.note.empty();
  const auto visible=[&](const char *field,bool show) {
    obs_property_set_visible(obs_properties_get(props,field),show);
  };
  for(const auto *field:{"waveform","one_shot","ease_in","ease_out","ease_in_help","duration","restore","midi_note_port",
                         "learn_note"})visible(field,!cc);
  visible("midi_note_channel",note_selected);
  visible("midi_note",note_selected);
  for(const auto *field:{"midi_clock_port","midi_division","midi_align","midi_meter_top","midi_meter_bottom"})visible(field,clock);
  for(const auto *field:{"midi_cc_port","midi_cc_channel","midi_cc","midi_pickup","learn_cc"})visible(field,cc);
  visible("midi_hold",note_selected && !cfg.one_shot);
  visible("period",!clock && !cc);
  visible("play",!cc && cfg.one_shot);
  visible("restart",!cc && !cfg.one_shot);
  obs_property_set_enabled(obs_properties_get(props,"ease_in"),!(cfg.one_shot && cfg.wave==5));
  obs_property_set_description(obs_properties_get(props,"duration"),clock ?
    (cfg.one_shot ? "One musical cycle, plus a separate return cycle if Ease out is enabled." : "Continuous modulation follows MIDI Clock.") : cfg.one_shot ?
    ("Total shot at the configured rate: "+std::to_string(cfg.period*(cfg.ease_out ? 2:1))+" seconds. Ease in shares the first cycle.").c_str()
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
  auto *mode=obs_properties_add_list(props,"midi_mode","Control mode",OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_INT);
  obs_property_list_add_int(mode,"Seconds",0);
  obs_property_list_add_int(mode,"MIDI Clock",1);
  obs_property_list_add_int(mode,"MIDI CC",2);
  obs_properties_add_float(props,"period","Cycle time (seconds)",.05,86400,.05);
  obs_properties_add_list(props,"midi_clock_port","Clock input",OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_STRING);
  auto *division=obs_properties_add_list(props,"midi_division","Cycle length",OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_INT);
  const char *divisions[]={"1 bar","Whole note","Half note","Quarter note","Eighth note","Sixteenth note"};
  for(int i=0;i<6;++i)obs_property_list_add_int(division,divisions[i],i);
  obs_properties_add_bool(props,"midi_align","Downbeat aligned");
  obs_properties_add_int(props,"midi_meter_top","Time signature numerator",1,32,1);
  auto *meter=obs_properties_add_list(props,"midi_meter_bottom","Time signature denominator",OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_INT);
  for(const int n:{1,2,4,8,16,32})obs_property_list_add_int(meter,std::to_string(n).c_str(),n);
  obs_properties_add_list(props,"midi_note_port","Note input",OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_STRING);
  obs_properties_add_int(props,"midi_note_channel","Note channel",1,16,1);
  auto *note=obs_properties_add_list(props,"midi_note","Note",OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_INT);
  for(int number=0;number<=127;++number)obs_property_list_add_int(note,midi_note_label(number).c_str(),number);
  obs_property_set_long_description(note,"Middle C is C3 (60) here. The MIDI number matches across different octave naming conventions.");
  obs_properties_add_bool(props,"midi_hold","Hold to run");
  obs_properties_add_button2(props,"learn_note","Learn note",learn_clicked,c);
  obs_properties_add_list(props,"midi_cc_port","CC input",OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_STRING);
  obs_properties_add_int(props,"midi_cc_channel","CC channel",1,16,1);
  obs_properties_add_int(props,"midi_cc","CC number",0,119,1);
  obs_properties_add_bool(props,"midi_pickup","Pickup");
  obs_properties_add_button2(props,"learn_cc","Learn CC",learn_clicked,c);
  obs_properties_add_button2(props,"cancel_learn","Cancel MIDI Learn",learn_clicked,c);
  obs_properties_add_button2(props,"midi_inputs","Refresh MIDI inputs",midi_inputs_clicked,c);
  obs_properties_add_bool(props,"one_shot","One shot");
  obs_properties_add_bool(props,"ease_in","Ease in");
  obs_properties_add_bool(props,"ease_out","Ease out");
  obs_properties_add_text(props,"ease_in_help","",OBS_TEXT_INFO);
  obs_properties_add_text(props,"duration","",OBS_TEXT_INFO);
  obs_properties_add_text(props,"feedback_notice","",OBS_TEXT_INFO);
  auto *allow = obs_properties_add_bool(props,"allow_feedback","I understand — allow this feedback loop");
  obs_property_set_modified_callback2(allow,feedback_changed,c);
  feedback_choices(props, {});
  obs_properties_add_button2(props,"play","Play / retrigger",play_clicked,c);
  obs_properties_add_button2(props,"restart","Restart cycle",restart_clicked,c);
  obs_properties_add_button2(props,"restore","Restore starting value and disable",restore_clicked,c);
  auto *s=obs_properties_add_text(props,"status","Disabled — configure the mapping, then enable the filter eye",OBS_TEXT_INFO);
  obs_properties_add_text(props,"help","Enable the eye or Play to run while the parent is showing (including previews). One shot stops at its endpoint; Ease out optionally returns home. Hiding interrupts a shot. Operator control always wins: target edits turn the eye off and cancel queued work. Target dialogs may not redraw continuously.",OBS_TEXT_INFO);
  obs_property_set_modified_callback2(src,target_changed,c);obs_property_set_modified_callback2(f,target_changed,c);
  obs_property_set_modified_callback2(key,property_changed,c);
  for(const char *field:{"one_shot","waveform","ease_out","period","midi_mode","midi_note_port"})
    obs_property_set_modified_callback2(obs_properties_get(props,field),playback_choices,c);
  auto initial=Data(obs_data_create(),obs_data_release);
  obs_data_set_double(initial.get(),"period",4);playback_choices(c,props,nullptr,initial.get());
  obs_property_set_visible(obs_properties_get(props,"cancel_learn"),false);
  if(c) {
    std::lock_guard<std::recursive_mutex> lock(c->mutex);
    obs_property_set_visible(obs_properties_get(props,"cancel_learn"),c->learn!=0);
    const auto description=c->learn ? c->midi_event : c->status;
    obs_property_set_description(s,description.c_str());
    auto self=own(obs_weak_source_get_source(c->self));
    if(self){auto d=settings(self.get());c->ui_source=obs_data_get_string(d.get(),"source_uuid");c->ui_filter=obs_data_get_string(d.get(),"filter_uuid");c->ui_key=obs_data_get_string(d.get(),"property");preserve_selection(src,c->ui_source.c_str(),"Missing source — select a replacement");choices(props,c,d.get());playback_choices(c,props,nullptr,d.get());
      port_choices(props,d.get(),midi_input.ports());}
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
  obs_data_set_default_int(d,"midi_division",3);
  obs_data_set_default_int(d,"midi_meter_top",4);
  obs_data_set_default_int(d,"midi_meter_bottom",4);
  obs_data_set_default_int(d,"midi_note_channel",1);
  obs_data_set_default_int(d,"midi_cc_channel",1);
  obs_data_set_default_bool(d,"midi_pickup",true);
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
  {
    std::lock_guard<std::mutex> lock(parameter_mutex);
    animated_parameters.erase((*holder)->identity);
  }
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
      row.enabled=obs_source_enabled(self.get());row.waveform=cfg.wave;row.feedback_loop=h->feedback_loop;
      row.low=animated_value(h->identity, "minimum", cfg.low);
      row.high=animated_value(h->identity, "maximum", cfg.high);
      if ((row.low != cfg.low || row.high != cfg.high) && row.low > row.high) std::swap(row.low, row.high);
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
      if(!h->midi_event.empty())row.reason+=" · "+h->midi_event;
      if(cfg.midi.mode==MidiMode::CC && !cfg.midi.cc.empty() && !midi_input.connected(cfg.midi.cc))
        row.reason+=" · CC input unavailable";
      if(cfg.midi.mode!=MidiMode::CC && !cfg.midi.note.empty() && !midi_input.connected(cfg.midi.note))
        row.reason+=" · Note input unavailable";
      if(cfg.midi.mode==MidiMode::Clock) {
        const auto clock=clock_for(cfg.midi,now);
        const char *label=clock.stopped ? "MIDI stopped" : clock.lost ? "Clock lost" :
          clock.aligned && clock.valid ? "Clock and downbeat" : clock.valid ? "Clock only" : "No clock";
        if(row.reason.find(label)==std::string::npos)row.reason+=" · "+std::string(label);
        if(!cfg.midi.clock.empty() && !midi_input.connected(cfg.midi.clock))row.reason+=" · Clock input unavailable";
      }
      const double motion_now=cfg.midi.mode==MidiMode::Clock ? h->musical_time : now;
      const bool compatible=cfg==h->last;
      if(compatible && cfg.midi.mode==MidiMode::CC && row.enabled && h->cc_claim && h->cc_caught)
        row.state=MonitorState::Running;
      else if(compatible && cfg.midi.mode==MidiMode::CC && row.enabled)row.state=MonitorState::Waiting;
      else if(compatible && cfg.midi.mode==MidiMode::Clock && row.enabled &&
              (h->cue || h->status.find("holding phase")!=std::string::npos))row.state=MonitorState::Waiting;
      else if(compatible && h->releasing && !(row.enabled && h->gate_changed))row.state=MonitorState::EasingOut;
      else if(row.enabled) {
        if(!obs_source_showing(parent.get()))row.state=MonitorState::Waiting;
        else if(!compatible || !h->active || h->gate_changed)row.state=MonitorState::Starting;
        else if(cfg.ease_in && !(cfg.one_shot && cfg.wave==5) && h->phase(motion_now)<1)row.state=MonitorState::EasingIn;
        else row.state=MonitorState::Running;
      } else if(compatible && h->stop_reason==Controller::StopReason::ExternalEdit)row.state=MonitorState::Paused;
      else if(compatible && h->stop_reason==Controller::StopReason::Invalid)row.state=MonitorState::Error;
      if(!row.enabled && compatible && h->stop_reason==Controller::StopReason::Completed)row.state=MonitorState::Completed;
      if(!row.enabled && compatible && h->stop_reason==Controller::StopReason::Cancelled)row.state=MonitorState::Cancelled;
      if(compatible && h->active) {
        row.timed=true;
        const double period = h->releasing ? h->release_period : h->applied_period;
        const double release_now=h->release_seconds ? now : motion_now;
        const double cycles = std::max(0.0, h->releasing ? (release_now-h->release_started)/period : h->phase(motion_now));
        row.progress = h->releasing || cfg.one_shot || row.state==MonitorState::EasingIn ?
          std::clamp(cycles,0.0,1.0) : std::fmod(cycles,1.0);
        const double seconds=cfg.midi.mode==MidiMode::Clock && (!h->releasing || !h->release_seconds) ?
          h->last_clock_period : period;
        row.phase_remaining = seconds*(1-row.progress);
        row.total_remaining = cfg.one_shot ? row.phase_remaining+(!h->releasing && cfg.ease_out ? seconds:0):0;
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
void monitor_test_midi(midi::Event event) { injected_midi.push_back(std::move(event)); }
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
  blog(LOG_INFO,"[obs-modulator] Loaded %s (LFO + monitor + nested modulation + MIDI)", MODULATOR_VERSION);
#else
  blog(LOG_INFO,"[obs-modulator] Loaded %s (LFO + nested modulation + MIDI)", MODULATOR_VERSION);
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
