// SPDX-FileCopyrightText: 2026 Sparky and OBS Modulator contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "midi-input.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <iterator>
#include <map>
#include <mutex>
#include <utility>

#if defined(__APPLE__)
#include <CoreMIDI/CoreMIDI.h>
#include <mach/mach_time.h>
#elif defined(_WIN32)
#include <windows.h>
#include <mmsystem.h>
#include <mmddk.h>
#elif defined(MODULATOR_MIDI_ALSA)
#include <alsa/asoundlib.h>
#include <thread>
#endif

namespace modulator::midi {
namespace {
#if defined(__APPLE__) || defined(_WIN32) || defined(MODULATOR_MIDI_ALSA)
constexpr std::size_t queue_limit = 2048;
#endif

#if defined(__APPLE__) || defined(_WIN32)
bool normalize(Event &event, unsigned status, unsigned number, unsigned value)
{
  switch (status) {
  case 0xf8: event.kind = Kind::Clock; return true;
  case 0xfa: event.kind = Kind::Start; return true;
  case 0xfb: event.kind = Kind::Continue; return true;
  case 0xfc: event.kind = Kind::Stop; return true;
  default: break;
  }

  event.channel = static_cast<int>(status & 0x0f) + 1;
  event.number = static_cast<int>(number & 0x7f);
  event.value = static_cast<int>(value & 0x7f);
  switch (status & 0xf0) {
  case 0x80: event.kind = Kind::NoteOff; return true;
  case 0x90: event.kind = event.value ? Kind::NoteOn : Kind::NoteOff; return true;
  case 0xb0: event.kind = Kind::ControlChange; return true;
  default: return false;
  }
}
#endif

#if defined(__APPLE__)
using Address = MIDIEndpointRef;

std::string endpoint_name(Address endpoint)
{
  CFStringRef name = nullptr;
  if (MIDIObjectGetStringProperty(endpoint, kMIDIPropertyDisplayName, &name) != noErr)
    MIDIObjectGetStringProperty(endpoint, kMIDIPropertyName, &name);
  if (!name) return "MIDI input";
  const auto size = CFStringGetMaximumSizeForEncoding(CFStringGetLength(name), kCFStringEncodingUTF8);
  std::string value(static_cast<std::size_t>(size + 1), '\0');
  if (CFStringGetCString(name, value.data(), size + 1, kCFStringEncodingUTF8))
    value.resize(value.find('\0'));
  else
    value = "MIDI input";
  CFRelease(name);
  return value;
}

double packet_time(MIDITimeStamp timestamp)
{
  if (!timestamp) return Input::now();
  static const auto seconds_per_tick = [] {
    mach_timebase_info_data_t info{};
    mach_timebase_info(&info);
    return double(info.numer) / double(info.denom) * 1e-9;
  }();
  const auto observed = mach_absolute_time();
  const auto difference = timestamp < observed ? -double(observed - timestamp) : 0.0;
  return Input::now() + difference * seconds_per_tick;
}
#elif defined(_WIN32)
using Address = UINT;

std::string utf8(const wchar_t *value)
{
  const auto length = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
  if (length <= 1) return {};
  std::string result(static_cast<std::size_t>(length), '\0');
  if (!WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), length, nullptr, nullptr))
    return {};
  result.pop_back();
  return result;
}

std::string interface_name(Address address)
{
  const auto device = reinterpret_cast<HMIDIIN>(static_cast<UINT_PTR>(address));
  DWORD byte_count = 0;
  if (midiInMessage(device, DRV_QUERYDEVICEINTERFACESIZE, reinterpret_cast<DWORD_PTR>(&byte_count), 0) ||
      byte_count < sizeof(wchar_t) || byte_count > 32768) return {};
  std::vector<wchar_t> buffer(byte_count / sizeof(wchar_t) + 1);
  if (midiInMessage(device, DRV_QUERYDEVICEINTERFACE, reinterpret_cast<DWORD_PTR>(buffer.data()), byte_count))
    return {};
  return utf8(buffer.data());
}
#elif defined(MODULATOR_MIDI_ALSA)
using Address = std::pair<int, int>;
#else
using Address = int;
#endif

struct Endpoint {
  Port port;
  Address address;
};

void remove_ambiguous(std::vector<Endpoint> &ports)
{
  std::map<std::string, std::size_t> counts;
  for (auto &endpoint : ports) ++counts[endpoint.port.id];
  ports.erase(std::remove_if(ports.begin(), ports.end(), [&](const Endpoint &entry) {
    return counts.at(entry.port.id) != 1;
  }), ports.end());
}
} // namespace

struct Input::Impl {
  struct Connection {
    Impl &owner;
    std::string id;
    Address address;
    std::atomic<bool> connected{false};
#if defined(__APPLE__)
    std::uintptr_t token = 0;
#elif defined(_WIN32)
    HMIDIIN handle = nullptr;
    std::atomic<double> started{0};
#endif
    Connection(Impl &input, const std::string &key, Address location)
      : owner(input), id(key), address(location) {}
  };

  std::mutex control;
  std::mutex queue_mutex;
  std::deque<Event> pending;
  std::map<std::string, double> disconnected;
  bool overflow = false;
  std::vector<Endpoint> endpoints;
  double scanned = -1e100;
  std::map<std::string, std::unique_ptr<Connection>> active;

#if defined(__APPLE__)
  MIDIClientRef client = 0;
  MIDIPortRef port = 0;
  // A unique token lets an in-flight callback detect that its connection
  // was removed without dereferencing a context freed on the control thread.
  std::mutex callback_mutex;
  std::map<std::uintptr_t, Connection *> callbacks;
  std::uintptr_t next_token = 0;
#elif defined(MODULATOR_MIDI_ALSA)
  snd_seq_t *sequence = nullptr;
  int port = -1;
  std::thread receiver;
  std::condition_variable receiver_wakeup;
  bool stopping = false;
#endif

  ~Impl()
  {
#if defined(MODULATOR_MIDI_ALSA)
    {
      std::lock_guard<std::mutex> lock(control);
      stopping = true;
    }
    receiver_wakeup.notify_one();
    if (receiver.joinable()) receiver.join();
#endif
    for (auto &entry : active) disconnect(*entry.second);
#if defined(__APPLE__)
    if (port) MIDIPortDispose(port);
    if (client) MIDIClientDispose(client);
#elif defined(MODULATOR_MIDI_ALSA)
    if (sequence) snd_seq_close(sequence);
#endif
  }

#if defined(__APPLE__) || defined(_WIN32) || defined(MODULATOR_MIDI_ALSA)
  void push(Connection &connection, Event event)
  {
    std::lock_guard<std::mutex> lock(queue_mutex);
    if (!connection.connected || overflow) return;
    if (pending.size() == queue_limit) {
      pending.clear();
      overflow = true;
      return;
    }
    pending.push_back(std::move(event));
  }
#endif

  void refresh()
  {
    const double time = Input::now();
    if (time - scanned < 0.5) return;
    scanned = time;
    endpoints.clear();
#if defined(__APPLE__)
    const auto total = MIDIGetNumberOfSources();
    for (ItemCount i = 0; i < total; ++i) {
      const auto endpoint = MIDIGetSource(i);
      SInt32 unique = 0, offline = 0;
      if (MIDIObjectGetIntegerProperty(endpoint, kMIDIPropertyOffline, &offline) == noErr && offline)
        continue;
      if (MIDIObjectGetIntegerProperty(endpoint, kMIDIPropertyUniqueID, &unique) != noErr || !unique)
        continue;
      endpoints.push_back({{"coremidi:" + std::to_string(unique), endpoint_name(endpoint)}, endpoint});
    }
#elif defined(_WIN32)
    const auto total = midiInGetNumDevs();
    for (UINT i = 0; i < total; ++i) {
      MIDIINCAPSW caps{};
      if (midiInGetDevCapsW(i, &caps, static_cast<UINT>(sizeof(caps)))) continue;
      auto name = utf8(caps.szPname);
      const auto device_path = interface_name(i);
      const auto id = device_path.empty() ? "winmm-name:" + name : "winmm:" + device_path;
      if (!name.empty()) endpoints.push_back({{id, std::move(name)}, i});
    }
#elif defined(MODULATOR_MIDI_ALSA)
    if (!open_alsa()) return;
    snd_seq_client_info_t *client_info;
    snd_seq_port_info_t *port_info;
    snd_seq_client_info_alloca(&client_info);
    snd_seq_port_info_alloca(&port_info);
    snd_seq_client_info_set_client(client_info, -1);
    const int self = snd_seq_client_id(sequence);
    while (snd_seq_query_next_client(sequence, client_info) >= 0) {
      const int client = snd_seq_client_info_get_client(client_info);
      if (client == self || client == SND_SEQ_CLIENT_SYSTEM) continue;
      const std::string client_name = snd_seq_client_info_get_name(client_info);
      snd_seq_port_info_set_client(port_info, client);
      snd_seq_port_info_set_port(port_info, -1);
      while (snd_seq_query_next_port(sequence, port_info) >= 0) {
        constexpr unsigned required = SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ;
        if ((snd_seq_port_info_get_capability(port_info) & required) != required) continue;
        const int source_port = snd_seq_port_info_get_port(port_info);
        const std::string port_name = snd_seq_port_info_get_name(port_info);
        const auto id = "alsa:" + std::to_string(client_name.size()) + ':' + client_name +
                        ':' + std::to_string(source_port) + ':' + port_name;
        endpoints.push_back({{id, client_name + " / " + port_name}, {client, source_port}});
      }
    }
#endif
    remove_ambiguous(endpoints);
  }

  void select(const std::set<std::string> &requested)
  {
    if (!requested.empty()) refresh();
    for (auto it = active.begin(); it != active.end();) {
      auto &connection = *it->second;
      const auto located = std::find_if(endpoints.begin(), endpoints.end(), [&](const Endpoint &endpoint) {
        return endpoint.port.id == connection.id && endpoint.address == connection.address;
      });
      if (requested.count(connection.id) && located != endpoints.end()) { ++it; continue; }
      connection.connected = false;
      disconnect(connection);
      {
        std::lock_guard<std::mutex> lock(queue_mutex);
        pending.erase(std::remove_if(pending.begin(), pending.end(), [&](const Event &event) {
          return event.port == connection.id;
        }), pending.end());
        disconnected.insert_or_assign(connection.id, Input::now());
      }
      it = active.erase(it);
    }
    for (const auto &endpoint : endpoints) {
      if (!requested.count(endpoint.port.id) || active.count(endpoint.port.id)) continue;
      auto connection = std::make_unique<Connection>(*this, endpoint.port.id, endpoint.address);
      connection->connected = true;
      if (connect(*connection))
        active.emplace(endpoint.port.id, std::move(connection));
    }
#if defined(MODULATOR_MIDI_ALSA)
    receiver_wakeup.notify_one();
#endif
  }

#if defined(__APPLE__)
  void receive(const MIDIEventList *packets, void *source)
  {
    std::lock_guard<std::mutex> lock(callback_mutex);
    const auto callback = callbacks.find(reinterpret_cast<std::uintptr_t>(source));
    if (callback == callbacks.end()) return;
    auto &connection = *callback->second;
    const MIDIEventPacket *packet = &packets->packet[0];
    for (UInt32 i = 0; i < packets->numPackets; ++i, packet = MIDIEventPacketNext(packet)) {
      const auto time = packet_time(packet->timeStamp);
      constexpr unsigned word_count[] = {1, 1, 1, 2, 2, 4, 1, 1, 2, 2, 2, 3, 3, 4, 4, 4};
      for (UInt32 word = 0; word < packet->wordCount;) {
        const auto message = packet->words[word];
        const auto type = message >> 28;
        word += word_count[type];
        if (type != 1 && type != 2) continue;
        Event event{connection.id, Kind::Disconnected, 0, 0, 0, time};
        if (normalize(event, (message >> 16) & 0xff, message >> 8, message))
          connection.owner.push(connection, std::move(event));
      }
    }
  }

  bool connect(Connection &connection)
  {
    if (!client && MIDIClientCreate(CFSTR("OBS Modulator"), nullptr, nullptr, &client) != noErr)
      return false;
    if (!port && MIDIInputPortCreateWithProtocol(client, CFSTR("Modulator input"), kMIDIProtocol_1_0,
        &port, ^(const MIDIEventList *events, void *source) { receive(events, source); }) != noErr)
      return false;
    {
      std::lock_guard<std::mutex> lock(callback_mutex);
      connection.token = ++next_token;
      callbacks.emplace(connection.token, &connection);
    }
    const auto cookie = reinterpret_cast<void *>(connection.token);
    if (MIDIPortConnectSource(port, connection.address, cookie) == noErr) return true;
    std::lock_guard<std::mutex> lock(callback_mutex);
    callbacks.erase(connection.token);
    return false;
  }

  void disconnect(Connection &connection)
  {
    {
      std::lock_guard<std::mutex> lock(callback_mutex);
      callbacks.erase(connection.token);
    }
    MIDIPortDisconnectSource(port, connection.address);
  }
#elif defined(_WIN32)
  static void CALLBACK receive(HMIDIIN, UINT kind, DWORD_PTR context, DWORD_PTR message, DWORD_PTR timestamp)
  {
    if (kind != MIM_DATA && kind != MIM_MOREDATA) return;
    auto &connection = *reinterpret_cast<Connection *>(context);
    const auto observed = Input::now();
    const double stamped = connection.started.load() + double(static_cast<DWORD>(timestamp)) / 1000.0;
    // WinMM timestamps wrap after ~50 days. Arrival time is still valid when
    // the timestamp has wrapped or the driver reports an inconsistent value.
    const double time = (stamped <= observed + .01 && stamped + 1 >= observed) ?
                        std::min(observed, stamped) : observed;
    Event event{connection.id, Kind::Disconnected, 0, 0, 0, time};
    if (normalize(event, static_cast<unsigned>(message & 0xff), static_cast<unsigned>(message >> 8),
                  static_cast<unsigned>(message >> 16)))
      connection.owner.push(connection, std::move(event));
  }

  bool connect(Connection &connection)
  {
    if (midiInOpen(&connection.handle, connection.address, reinterpret_cast<DWORD_PTR>(receive),
                   reinterpret_cast<DWORD_PTR>(&connection), CALLBACK_FUNCTION | MIDI_IO_STATUS))
      return false;
    connection.started = Input::now();
    if (midiInStart(connection.handle) == MMSYSERR_NOERROR) return true;
    midiInClose(connection.handle);
    return false;
  }

  void disconnect(Connection &connection)
  {
    midiInStop(connection.handle);
    midiInReset(connection.handle);
    midiInClose(connection.handle);
  }
#elif defined(MODULATOR_MIDI_ALSA)
  bool open_alsa()
  {
    if (sequence) return true;
    if (snd_seq_open(&sequence, "default", SND_SEQ_OPEN_INPUT, SND_SEQ_NONBLOCK) < 0) {
      sequence = nullptr;
      return false;
    }
    snd_seq_set_client_name(sequence, "OBS Modulator");
    port = snd_seq_create_simple_port(sequence, "Modulator input",
        SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE, SND_SEQ_PORT_TYPE_APPLICATION);
    if (port < 0) {
      snd_seq_close(sequence);
      sequence = nullptr;
      return false;
    }
    return true;
  }

  void read_alsa()
  {
    for (std::size_t i = 0; i < queue_limit; ++i) {
      snd_seq_event_t *raw = nullptr;
      const int status = snd_seq_event_input(sequence, &raw);
      if (status < 0 || !raw) {
        if (status == -ENOSPC) {
          std::lock_guard<std::mutex> lock(queue_mutex);
          pending.clear();
          overflow = true;
        }
        break;
      }
      const Address address{raw->source.client, raw->source.port};
      auto selected = std::find_if(active.begin(), active.end(), [&](const auto &entry) {
        return entry.second->address == address;
      });
      if (selected == active.end()) continue;
      auto &connection = *selected->second;
      Event event{connection.id, Kind::Disconnected, 0, 0, 0, Input::now()};
      switch (raw->type) {
      case SND_SEQ_EVENT_CLOCK: event.kind = Kind::Clock; break;
      case SND_SEQ_EVENT_START: event.kind = Kind::Start; break;
      case SND_SEQ_EVENT_CONTINUE: event.kind = Kind::Continue; break;
      case SND_SEQ_EVENT_STOP: event.kind = Kind::Stop; break;
      case SND_SEQ_EVENT_NOTEON:
      case SND_SEQ_EVENT_NOTEOFF:
        event.kind = raw->type == SND_SEQ_EVENT_NOTEON && raw->data.note.velocity ? Kind::NoteOn : Kind::NoteOff;
        event.channel = int(raw->data.note.channel) + 1;
        event.number = raw->data.note.note;
        event.value = raw->data.note.velocity;
        break;
      case SND_SEQ_EVENT_CONTROLLER:
        event.kind = Kind::ControlChange;
        if (raw->data.control.param > 127 || raw->data.control.value < 0 || raw->data.control.value > 127)
          continue;
        event.channel = int(raw->data.control.channel) + 1;
        event.number = static_cast<int>(raw->data.control.param);
        event.value = raw->data.control.value;
        break;
      default: continue;
      }
      push(connection, std::move(event));
    }
  }

  bool connect(Connection &connection)
  {
    const auto &address = connection.address;
    if (snd_seq_connect_from(sequence, port, address.first, address.second) < 0) return false;
    if (!receiver.joinable()) {
      receiver = std::thread([this] {
        std::unique_lock<std::mutex> lock(control);
        while (!stopping) {
          if (active.empty()) {
            receiver_wakeup.wait(lock, [this] { return stopping || !active.empty(); });
            continue;
          }
          read_alsa();
          receiver_wakeup.wait_for(lock, std::chrono::milliseconds(2));
        }
      });
    }
    return true;
  }

  void disconnect(Connection &connection)
  {
    snd_seq_disconnect_from(sequence, port, connection.address.first, connection.address.second);
  }
#else
  bool connect(Connection &) { return false; }
  void disconnect(Connection &) {}
#endif
};

Input::Input() : impl_(std::make_unique<Impl>()) {}
Input::~Input() = default;

double Input::now()
{
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

std::vector<Port> Input::ports()
{
  std::lock_guard<std::mutex> lock(impl_->control);
  impl_->refresh();
  std::vector<Port> result;
  result.reserve(impl_->endpoints.size());
  for (const auto &endpoint : impl_->endpoints) result.push_back(endpoint.port);
  return result;
}

void Input::select(const std::set<std::string> &requested)
{
  std::lock_guard<std::mutex> lock(impl_->control);
  impl_->select(requested);
}

bool Input::connected(const std::string &port) const
{
  std::lock_guard<std::mutex> lock(impl_->control);
  return impl_->active.count(port) != 0;
}

std::vector<Event> Input::drain()
{
  std::lock_guard<std::mutex> control_lock(impl_->control);
  std::lock_guard<std::mutex> queue_lock(impl_->queue_mutex);
  std::vector<Event> result;
  if (impl_->overflow) {
    impl_->pending.clear();
    impl_->overflow = false;
    const auto time = now();
    for (const auto &entry : impl_->active)
      impl_->disconnected.insert_or_assign(entry.first, time);
  }
  result.reserve(impl_->disconnected.size() + impl_->pending.size());
  for (const auto &entry : impl_->disconnected)
    result.push_back({entry.first, Kind::Disconnected, 0, 0, 0, entry.second});
  impl_->disconnected.clear();
  std::move(impl_->pending.begin(), impl_->pending.end(), std::back_inserter(result));
  impl_->pending.clear();
  return result;
}

} // namespace modulator::midi
