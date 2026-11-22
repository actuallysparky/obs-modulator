// SPDX-FileCopyrightText: 2026 Sparky and OBS Modulator contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "midi-input.hpp"
#include <CoreMIDI/CoreMIDI.h>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <thread>
#include <unistd.h>

using namespace modulator::midi;

int main()
{
  MIDIClientRef source_client = 0;
  MIDIEndpointRef source = 0;
  const auto name = "Modulator isolated MIDI smoke " + std::to_string(getpid());
  auto cf_name = CFStringCreateWithCString(nullptr, name.c_str(), kCFStringEncodingUTF8);
  const auto client_status = MIDIClientCreate(cf_name, nullptr, nullptr, &source_client);
  const auto port_status = client_status ? client_status :
    MIDISourceCreateWithProtocol(source_client, cf_name, kMIDIProtocol_1_0, &source);
  CFRelease(cf_name);
  if (port_status != noErr) {
    std::cerr << "Virtual endpoint creation failed: " << port_status << '\n';
    if (source_client) MIDIClientDispose(source_client);
    return 1;
  }
  struct Cleanup {
    MIDIClientRef client;
    ~Cleanup() { MIDIClientDispose(client); }
  } cleanup{source_client};

  Input input;
  Port selected;
  for (int i = 0; i < 20 && selected.id.empty(); ++i) {
    const auto ports = input.ports();
    const auto port = std::find_if(ports.begin(), ports.end(), [&](const auto &p) { return p.name == name; });
    if (port != ports.end()) selected = *port;
    else std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  if (selected.id.empty()) {
    std::cerr << "Virtual endpoint missing from enumeration\n";
    return 2;
  }
  input.select({selected.id});
  if (!input.connected(selected.id)) {
    std::cerr << "Could not open the enumerated virtual endpoint\n";
    return 3;
  }
  const UInt32 values[] = {0x10fa0000, 0x10f80000, 0x20923c64, 0x20b2107f, 0x20923c00, 0x10fc0000, 0x10fb0000};
  const Kind expected[] = {Kind::Start, Kind::Clock, Kind::NoteOn, Kind::ControlChange, Kind::NoteOff, Kind::Stop, Kind::Continue};
  MIDIEventList messages{};
  auto packet = MIDIEventListInit(&messages, kMIDIProtocol_1_0);
  packet = MIDIEventListAdd(&messages, sizeof(messages), packet, 0, 7, values);
  if (!packet || MIDIReceivedEventList(source, &messages) != noErr) {
    std::cerr << "Could not publish the MIDI fixture messages\n";
    return 4;
  }
  std::vector<Event> observed;
  for (int i = 0; i < 20 && observed.size() < 7; ++i) {
    auto batch = input.drain();
    observed.insert(observed.end(), batch.begin(), batch.end());
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (observed.size() != 7) {
    std::cerr << "Received " << observed.size() << " of 7 events\n";
    return 5;
  }
  for (size_t i = 0; i < observed.size(); ++i) {
    if (observed[i].kind != expected[i] || observed[i].port != selected.id ||
        observed[i].time <= 0 || observed[i].time > Input::now()) return 6;
  }
  if (observed[2].channel != 3 || observed[2].number != 60 || observed[2].value != 100 ||
      observed[3].channel != 3 || observed[3].number != 16 || observed[3].value != 127)
    return 7;
  MIDIEndpointDispose(source);
  std::this_thread::sleep_for(std::chrono::milliseconds(600));
  input.select({selected.id});
  const auto disconnected = input.drain();
  if (input.connected(selected.id) || disconnected.size() != 1 || disconnected[0].kind != Kind::Disconnected ||
      disconnected[0].port != selected.id)
    return 8;
  std::cout << "Native CoreMIDI input passed clock/transport, notes, CC, and disappearance checks\n";
}
