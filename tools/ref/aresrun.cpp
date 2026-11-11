/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

// aresrun game frames: unmodified ares (its dot renderer) with no frontend, saying what came out in
// the same form as tests/snesrun.c, for the two to be compared.
// env: FRAMELOG, AUDIOOUT, INPUT=1, RAWOUT and RAWFRAME, RESETAT, SAVEIN (the save RAM it starts with)
#include <ares/ares.hpp>
#include <mia/mia.hpp>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace ares::SuperFamicom {
auto load(Node::System&, string) -> bool;
auto option(string, string) -> bool;
}

static uint64_t fnv(uint64_t hash, const void* data, size_t size) {
  auto bytes = static_cast<const unsigned char*>(data);
  while(size--) { hash ^= *bytes++; hash *= 1099511628211ull; }
  return hash;
}

struct Runner : ares::Platform {
  std::shared_ptr<mia::Pak> game;
  std::shared_ptr<vfs::directory> system = std::make_shared<vfs::directory>();
  FILE* frameLog = nullptr;
  FILE* audioOut = nullptr;
  uint64_t audioHash = 14695981039346656037ull, lastHash = 0, samples = 0, frames = 0;
  std::atomic<uint64_t> shown = 0;
  bool pressing = false;
  unsigned rows = 484;

  auto pak(ares::Node::Object node) -> std::shared_ptr<vfs::directory> override {
    if(node->name() == "Super Famicom") return system;
    if(node->name() == "Super Famicom Cartridge") return game->pak;
    return {};
  }

  auto event(ares::Event event) -> void override {
    if(event == ares::Event::Frame) frames++;
  }

  //called on the screen's own thread, once for every frame in order, before the screen clears the
  //canvas it was drawn on: the only moment the finished canvas can be read safely
  auto video(ares::Node::Video::Screen screen, const u32*, u32, u32, u32) -> void override {
    auto pixels = screen->pixels(1);
    lastHash = fnv(14695981039346656037ull, pixels.data(), 564 * rows * sizeof(u32));
    shown++;
    if(frameLog) std::fprintf(frameLog, "%llu %016llx\n", (unsigned long long)shown.load(), (unsigned long long)lastHash);
    if(auto path = std::getenv("RAWOUT"); path && shown == std::strtoull(std::getenv("RAWFRAME"), nullptr, 10)) {
      if(FILE* out = std::fopen(path, "wb")) { std::fwrite(pixels.data(), sizeof(u32), 564 * rows, out); std::fclose(out); }
    }
  }

  auto audio(ares::Node::Audio::Stream stream) -> void override {
    double data[8];
    while(stream->pending()) {
      stream->read(data);
      int16_t pair[2] = {(int16_t)std::lrint(data[0] * 32768.0), (int16_t)std::lrint(data[1] * 32768.0)};
      audioHash = fnv(audioHash, pair, sizeof(pair));
      samples++;
      if(audioOut) std::fwrite(pair, sizeof(pair), 1, audioOut);
    }
  }

  auto held(const string& name) -> bool {
    if(!pressing) return false;
    unsigned phase = frames % 240;
    if(phase >=  60 && phase <  66) return name == "Start";
    if(phase >= 120 && phase < 126) return name == "A";
    if(phase >= 150 && phase < 170) return name == "Right";
    if(phase >= 180 && phase < 186) return name == "B";
    if(phase >= 200 && phase < 220) return name == "Down";
    return false;
  }

  auto input(ares::Node::Input::Input node) -> void override {
    auto button = node->cast<ares::Node::Input::Button>();
    if(!button) return;
    bool first = false;
    if(auto pad = node->parent().lock()) {
      if(auto port = pad->parent().lock()) first = port->name() == "Controller Port 1";
    }
    button->setValue(first && held(node->name()));
  }
};

#include <nall/main.hpp>
auto nall::main(Arguments arguments) -> void {
  std::vector<string> args;
  for(auto& argument : arguments) args.push_back(argument);
  if(args.size() != 2) { std::fprintf(stderr, "usage: aresrun game frames\n"); std::exit(2); }

  Runner runner;
  ares::platform = &runner;
  runner.game = mia::Medium::create("Super Famicom");
  if(!runner.game || runner.game->load(args[0]) != successful) { std::fprintf(stderr, "can't load the game\n"); std::exit(1); }
  std::printf("board=%s region=%s\n", runner.game->pak->attribute("board").data(), runner.game->pak->attribute("region").data());

  runner.system->append("ipl.rom", Resource::SuperFamicom::IPLROM);
  runner.system->append("boards.bml", file::read(ARES_BOARDS));
  ares::SuperFamicom::option("Pixel Accuracy", "true");
  ares::SuperFamicom::option("Deterministic Entropy", "true");

  bool pal = runner.game->pak->attribute("region") == "PAL";
  runner.rows = (pal ? 288 : 242) * 2;
  ares::Node::System root;
  if(!ares::SuperFamicom::load(root, pal ? "[Nintendo] Super Famicom (PAL)" : "[Nintendo] Super Famicom (NTSC)")) std::exit(1);
  if(auto path = std::getenv("SAVEIN")) {
    auto save = file::read(path);
    if(auto fp = runner.game->pak->read("save.ram")) runner.game->pak->remove(fp);
    runner.game->pak->append("save.ram", std::span<const u8>{save.data(), save.size()});
  }
  auto slot = root->find<ares::Node::Port>("Cartridge Slot");
  auto cartridge = slot->allocate();
  slot->connect();
  if(!cartridge || !cartridge->pak()) { std::fprintf(stderr, "the cartridge didn't connect\n"); std::exit(1); }
  for(const char* name : {"Controller Port 1", "Controller Port 2"}) {
    if(auto port = root->find<ares::Node::Port>(name)) { port->allocate("Gamepad"); port->connect(); }
  }
  //no resampling: the samples come out as the DSP made them, after two of silence
  for(auto& stream : root->find<ares::Node::Audio::Stream>()) stream->setResamplerFrequency(stream->frequency());

  if(auto path = std::getenv("FRAMELOG")) runner.frameLog = std::fopen(path, "w");
  if(auto path = std::getenv("AUDIOOUT")) runner.audioOut = std::fopen(path, "wb");
  runner.pressing = std::getenv("INPUT") != nullptr;

  root->power();
  uint64_t count = std::strtoull(args[1], nullptr, 10);
  uint64_t resetAt = std::getenv("RESETAT") ? std::strtoull(std::getenv("RESETAT"), nullptr, 10) : 0;
  while(runner.frames < count) {
    root->run();
    if(resetAt && runner.frames == resetAt) { root->power(true); resetAt = 0; }
  }
  while(runner.shown < count) std::this_thread::yield();

  std::printf("frames=%llu samples=%llu video=%016llx audio=%016llx\n", (unsigned long long)runner.frames,
    (unsigned long long)runner.samples, (unsigned long long)runner.lastHash, (unsigned long long)runner.audioHash);
  if(runner.frameLog) std::fclose(runner.frameLog);
  if(runner.audioOut) std::fclose(runner.audioOut);
  std::fflush(stdout);
  std::quick_exit(0);
}
