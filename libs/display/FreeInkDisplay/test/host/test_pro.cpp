#include <cassert>
#include <algorithm>
#include <vector>
#include <cstdio>
#include <cstring>
#include <type_traits>
// Inspect private state without adding test-only methods to the SDK API.
#define private public
#include "FreeInkDisplay.h"
#include "src/driver/Ssd1677Driver.h"
#include "src/lut/Ssd1677Luts.h"
#include "src/driver/Uc8179Driver.h"
#include "src/driver/Uc8279X4Driver.h"
#undef private

using namespace freeink;
using Bytes = std::vector<uint8_t>;

static uint8_t lastRegister(const EpdBus& bus, uint8_t command) {
  for (auto i = bus.writes.rbegin(); i != bus.writes.rend(); ++i)
    if (i->command == command && !i->bytes.empty()) return i->bytes[0];
  assert(false);
  return 0;
}

static const Bytes& lastPlane(const EpdBus& bus, uint8_t command) {
  for (auto i = bus.writes.rbegin(); i != bus.writes.rend(); ++i)
    if (i->command == command) return i->bytes;
  assert(false);
  static const Bytes empty;
  return empty;
}

static Bytes frame(unsigned seed) {
  Bytes b(48000);
  for (size_t i=0; i<b.size(); ++i) b[i]=uint8_t((i*37 + i/100*11 + seed) ^ (i>>8));
  return b;
}

template<class Driver>
static void testStream(bool reverse, unsigned gateOffset) {
  Driver d;
  EpdBus bus;
  const auto a=frame(3), b=frame(89);
  for (bool invert : {false,true}) {
    bus.clear();
    d.streamPlane(bus,0x13,a.data(),invert);
    assert(bus.writes.size()==1);
    const auto& w=bus.writes.front();
    assert(w.transactions==1 && w.bytes.size()==60000);
    for (unsigned y=0; y<600; ++y) for (unsigned x=0; x<100; ++x) {
      uint8_t expected=0xff;
      if (y>=gateOffset && y<gateOffset+480) {
        unsigned row=y-gateOffset;
        if (reverse) row=479-row;
        expected=a[row*100+x];
        if (invert) expected=uint8_t(~expected);
      }
      assert(w.bytes[y*100+x]==expected);
    }
  }
  bus.clear();
  d.streamPlaneXor(bus,0x13,a.data(),b.data());
  assert(bus.writes.size()==1 && bus.writes[0].transactions==1);
  assert(bus.writes[0].bytes.size()==60000);
  for (unsigned y=0; y<600; ++y) for (unsigned x=0; x<100; ++x) {
    uint8_t expected=0xff;
    if (y>=gateOffset && y<gateOffset+480) {
      unsigned row=y-gateOffset;
      if (reverse) row=479-row;
      expected=a[row*100+x]^b[row*100+x];
    }
    assert(bus.writes[0].bytes[y*100+x]==expected);
  }
  d._grayRefreshedOnce=true;
  d._oldPlaneValid=true;
  d._needFullClear=false;
  bus.clear();
  d.displayGrayscaleBase(bus,a.data(),RefreshMode::Full,false);
  assert(lastRegister(bus,0xe5)==0x1e); // honor Full even after an AA page
  for (const auto& w : bus.writes)
    if (w.command==0x10 || w.command==0x13) assert(w.transactions==1);
}

static void testSsd() {
  const auto b=frame(33);
  for (auto mode : {RefreshMode::Full,RefreshMode::Half,RefreshMode::Fast}) {
    EpdBus bus;
    Ssd1677Driver d;
    d.begin(bus);
    bus.clear();
    d.display(bus,b.data(),nullptr,mode,false);
    assert(lastRegister(bus,0x22)==(mode==RefreshMode::Full ? 0xf7 : 0xd7));
    bus.clear();
    d.display(bus,b.data(),nullptr,RefreshMode::Fast,false);
    assert(lastRegister(bus,0x22)==0xfc);
  }
  // The first-paint-after-boot/wake promotion must survive turnOff. A caller that powers
  // the panel down on every refresh (CrossPoint's sunlight fading fix) otherwise never
  // consumes the one-shot, and its differential FAST cannot clear the sleep screen.
  {
    EpdBus bus;
    Ssd1677Driver d;
    d.begin(bus);
    // The activation value is not the LAST 0x22 write here: 0xfc does not self-power-off,
    // so the driver follows it with the separate 0x22=0x03 power-down sequence.
    const auto activated = [&bus](uint8_t seq) {
      for (const auto& w : bus.writes)
        if (w.command == 0x22 && !w.bytes.empty() && w.bytes[0] == seq) return true;
      return false;
    };
    bus.clear();
    d.display(bus,b.data(),nullptr,RefreshMode::Fast,true);
    assert(activated(0xd7) && !activated(0xfc));
    bus.clear();
    d.display(bus,b.data(),nullptr,RefreshMode::Fast,true);
    assert(activated(0xfc) && !activated(0xd7));
  }
  EpdBus bus;
  Ssd1677Driver d;
  d.begin(bus);
  bus.clear();
  d.displayGray(bus,b.data(),false,nullptr,false);
  assert(lastRegister(bus,0x22)==0xcc && d._isScreenOn);
  d.deepSleep(bus);
  assert(lastRegister(bus,0x22)==3 && !d._isScreenOn);
  bus.clear();
  d.displayGray(bus,b.data(),true,nullptr,false);
  assert(lastRegister(bus,0x22)==0xcf && !d._isScreenOn);
}

template<class Driver>
static void testAsyncFrame() {
  Driver driver;
  FreeInkDisplay display(12,11,13,18,14,6);
  display._driver=&driver;
  display.begin();
  const auto submitted=frame(27), redrawn=frame(94);
  std::memcpy(display.getFrameBuffer(),submitted.data(),submitted.size());
  display.displayBufferAsync(FreeInkDisplay::FAST_REFRESH);
  assert(display.isRefreshPending());
  std::memcpy(display.getFrameBuffer(),redrawn.data(),redrawn.size());
  display.completeDisplay();
  // Finish must sync the frame actually submitted, even after the caller draws.
  const auto& old=display._bus.writes.back();
  assert(old.command==0x10 && old.bytes.size()==60000);
  for (unsigned y=0; y<480; ++y) {
    unsigned dst;
    if constexpr (std::is_same<Driver,Uc8179Driver>::value) dst=479-y;
    else dst=120+y;
    assert(std::equal(submitted.begin()+y*100,submitted.begin()+(y+1)*100,old.bytes.begin()+dst*100));
  }
  assert(!display.isRefreshPending());
  // Shadow-free entry points require the live frame to survive until finish.
  std::memcpy(display.getFrameBuffer(),redrawn.data(),redrawn.size());
  display.triggerDisplay(FreeInkDisplay::FAST_REFRESH,false);
  display.completeDisplay();
  const auto& next=display._bus.writes.back();
  unsigned firstRow=std::is_same<Driver,Uc8179Driver>::value ? 479 : 0;
  unsigned offset=std::is_same<Driver,Uc8179Driver>::value ? 0 : 12000;
  assert(std::equal(redrawn.begin()+firstRow*100,redrawn.begin()+(firstRow+1)*100,next.bytes.begin()+offset));
#ifdef EINK_DISPLAY_SINGLE_BUFFER_MODE
  std::memcpy(display.getFrameBuffer(),submitted.data(),submitted.size());
  display.displayAsyncImpl(FreeInkDisplay::FAST_REFRESH,false,true);
  display.completeDisplay();
  const auto& noShadow=display._bus.writes.back();
  assert(std::equal(submitted.begin()+firstRow*100,submitted.begin()+(firstRow+1)*100,
                    noShadow.bytes.begin()+offset));
#endif
  display.releaseBuffers();
  free(driver._grayBase);
}

// A driver with no grayscale implementation must never advertise support.
class BwOnlyDriver : public PanelDriver {
 public:
  uint32_t spiHz() const override { return 10000000; }
  BusyPolarity busyPolarity() const override { return BusyPolarity::ActiveHigh; }
  PanelGeometry geometry() const override { return {800, 480, 100, 48000}; }
  void begin(EpdBus&) override {}
  void deepSleep(EpdBus&) override {}
  void display(EpdBus&, const uint8_t*, const uint8_t*, RefreshMode, bool) override {}
};

class CombinedGrayDriver : public BwOnlyDriver {
 public:
  GrayscaleCapabilities grayscaleCapabilities(GrayscaleMode mode = GrayscaleMode::Overlay) const override {
    if (mode != GrayscaleMode::Overlay) return {};
    return {GrayscaleEncoding::OverlayMasks, GrayscaleBase::Combined, true, false, true};
  }
};

static void testCapabilities() {
  FreeInkDisplay display(1, 2, 3, 4, 5, 6);
  assert(!display.grayscaleCapabilities().supported());
  BwOnlyDriver bw;
  display._driver = &bw;
  assert(!display.grayscaleCapabilities().supported());
  assert(!display.supportsStripGrayscale());

  Ssd1677Driver ssd;
  display._driver = &ssd;
  auto caps = display.grayscaleCapabilities();
  assert(caps.supported() && caps.encoding == GrayscaleEncoding::OverlayMasks);
  assert(caps.stripUploads && caps.asyncBase && !caps.stagingWhileBusy);
  assert(caps.base == GrayscaleBase::Separate);
  assert(display.supportsStripGrayscale() == caps.stripUploads);
  assert(display.supportsAsyncGrayscaleBase() == caps.asyncBase);
  assert(display.grayscaleCapabilities(GrayscaleMode::Absolute).supported());
  assert(!display.grayscaleCapabilities(static_cast<GrayscaleMode>(255)).supported());
  display._inversionDirty = true;
  assert(!display.grayscaleCapabilities().asyncBase);
  assert(display.grayscaleCapabilities().stripUploads);
  assert(!display.supportsAsyncGrayscaleBase());
  display._inversionDirty = false;

  CombinedGrayDriver combined;
  display._driver = &combined;
  assert(display.combinesGrayscaleBase());
  assert(display.supportsBusyGrayscaleStaging());
  assert(!display.supportsAsyncGrayscaleBase());
  display._inverted = true;
  caps = display.grayscaleCapabilities();
  assert(!caps.supported() && !caps.stripUploads && !caps.asyncBase && !caps.stagingWhileBusy);
  assert(!display.combinesGrayscaleBase() && !display.supportsBusyGrayscaleStaging());
  assert(!display.supportsStripGrayscale());
  display._inverted = false;

  Uc8179Driver uc8179;
  Uc8279X4Driver uc8279;
  for (PanelDriver* driver : {static_cast<PanelDriver*>(&uc8179), static_cast<PanelDriver*>(&uc8279)}) {
    display._driver = driver;
    caps = display.grayscaleCapabilities();
    assert(caps.supported() && !caps.stripUploads && !caps.asyncBase && !caps.stagingWhileBusy);
    assert(display.grayscaleCapabilities(GrayscaleMode::Absolute).supported());
    assert(!display.grayscaleCapabilities(GrayscaleMode::Absolute).stripUploads);
  }
  // Queries must neither start a refresh nor write the panel bus.
  assert(display._bus.writes.empty() && !display.isRefreshPending());
}

static void testAbsolutePipeline() {
  Ssd1677Driver driver;
  FreeInkDisplay display(1, 2, 3, 4, 5, 6);
  display._driver = &driver;
  display.begin();
  const auto bw = frame(33), lsb = frame(14), msb = frame(29);
  std::memcpy(display.getFrameBuffer(), bw.data(), bw.size());
  const auto absolute = GrayscaleMode::Absolute;
  assert(display.grayscaleCapabilities(absolute).base == GrayscaleBase::Combined);
  for (bool strips : {false, true}) {
    display._bus.clear();
    assert(display.displayGrayscaleBase(absolute));
    assert(display._bus.writes.empty());
    if (strips) {
      for (unsigned y = 0; y < 480; y += 80) {
        display.writeGrayscalePlaneStrip(FreeInkDisplay::GRAY_PLANE_LSB, lsb.data() + y * 100, y, 80);
        display.writeGrayscalePlaneStrip(FreeInkDisplay::GRAY_PLANE_MSB, msb.data() + y * 100, y, 80);
      }
    } else display.copyGrayscaleBuffers(lsb.data(), msb.data());
    Bytes plane0, plane1;
    for (const auto& w : display._bus.writes) {
      if (w.command == 0x24) plane0.insert(plane0.end(), w.bytes.begin(), w.bytes.end());
      if (w.command == 0x26) plane1.insert(plane1.end(), w.bytes.begin(), w.bytes.end());
    }
    assert(plane0.size() == lsb.size() && plane1.size() == msb.size());
    for (size_t i = 0; i < lsb.size(); ++i) {
      assert(plane0[i] == uint8_t(~lsb[i]) && plane1[i] == uint8_t(~msb[i]));
    }
    for (const auto& w : display._bus.writes) assert(w.command != 0x20);
    display.displayGrayBuffer(false);
    assert(std::count_if(display._bus.writes.begin(), display._bus.writes.end(),
                         [](const auto& w) { return w.command == 0x20; }) == 1);
    assert(lastRegister(display._bus, 0x22) == 0xCC);
    assert(driver._isScreenOn && driver._needsGrayClear);
    display.cleanupGrayscaleBuffers(bw.data());
    assert(driver._needsGrayClear);
    display._bus.clear();
    display.displayBufferAsync(FreeInkDisplay::FAST_REFRESH);
    assert(lastRegister(display._bus, 0x22) == 0xD7);
    display.waitRefreshComplete();
    assert(!driver._needsGrayClear);
  }
  // Explicit shutdown must use its own activation after the gray refresh wait.
  assert(display.displayGrayscaleBase(absolute));
  display.copyGrayscaleBuffers(lsb.data(), msb.data());
  display._bus.clear();
  display.displayGrayBuffer(true);
  std::vector<uint8_t> activations;
  for (const auto& w : display._bus.writes) {
    if (w.command == 0x22) activations.push_back(w.bytes.at(0));
  }
  assert((activations == std::vector<uint8_t>{0xCC, 0x03}));
  assert(!driver._isScreenOn && driver._needsGrayClear);
  for (unsigned failure = 0; failure < 6; ++failure) {
    assert(display.displayGrayscaleBase(absolute));
    display._bus.clear();
    if (failure == 0) display.copyGrayscaleLsbBuffers(lsb.data()); // missing MSB
    if (failure == 1) display.writeGrayscalePlaneStrip(FreeInkDisplay::GRAY_PLANE_LSB, lsb.data(), 80, 80);
    if (failure == 2) display.writeGrayscalePlaneStrip(FreeInkDisplay::GRAY_PLANE_LSB, lsb.data(), 0, 481);
    if (failure == 3) display.copyGrayscaleLsbBuffers(nullptr);
    if (failure == 4) {
      display.copyGrayscaleBuffers(lsb.data(), msb.data());
      display.copyGrayscaleLsbBuffers(lsb.data()); // duplicate plane
    }
    if (failure == 5) {
      display.copyGrayscaleMsbBuffers(msb.data());
      display.copyGrayscaleLsbBuffers(lsb.data());
    }
    display.displayGrayBuffer();
    for (const auto& w : display._bus.writes) assert(w.command != 0x20);
    display.cleanupGrayscaleBuffers(nullptr);
    display._bus.clear();
    display.displayBuffer(FreeInkDisplay::FAST_REFRESH, true);
    assert(lastRegister(display._bus, 0x22) == 0xD7);
  }
  assert(display.displayGrayscaleBase(absolute));
  display.copyGrayscaleLsbBuffers(lsb.data());
  display.setInverted(true);
  assert(display._grayscaleMode == GrayscaleMode::Overlay);
  assert(!display.displayGrayscaleBase(absolute));
  display.setInverted(false);
  display._bus.clear();
  assert(display.displayGrayscaleBase(absolute));
  assert(display._bus.writes.empty() && display._inversionDirty);
  display.copyGrayscaleBuffers(lsb.data(), msb.data());
  display.displayGrayBuffer();
  assert(!display._inversionDirty);
  assert(display.displayGrayscaleBase(absolute));
  display.deepSleep();
  assert(display._grayscaleMode == GrayscaleMode::Overlay);
  display.releaseBuffers();

  Ssd1677Config unsupported = ssd1677DefaultConfig();
  unsupported.absoluteGrayscale = false;
  Ssd1677Driver other(unsupported);
  FreeInkDisplay noAbsolute(1, 2, 3, 4, 5, 6);
  noAbsolute._driver = &other;
  assert(!noAbsolute.displayGrayscaleBase(absolute));
  assert(noAbsolute._bus.writes.empty());
}

template<class Driver>
static void testUltraChipAbsolute(bool reverse, unsigned gateOffset, bool inverted) {
  for (bool snapshot : {false, true}) for (bool turnOff : {false, true}) {
    Driver driver;
    FreeInkDisplay display(1, 2, 3, 4, 5, 6);
    display._driver = &driver;
    display.begin();
    if (!snapshot) { free(driver._grayBase); driver._grayBase = nullptr; }
    const auto bw = frame(91), lsb = frame(13), msb = frame(29);
    memcpy(display.getFrameBuffer(), bw.data(), bw.size());
    assert(display.displayGrayscaleBase(GrayscaleMode::Absolute));
    display._bus.clear();
    display.copyGrayscaleBuffers(lsb.data(), msb.data());
    for (unsigned p = 0; p < 2; ++p) {
      const auto& input = p ? msb : lsb;
      const auto command = p ? 0x13 : 0x10;
      const auto it = std::find_if(display._bus.writes.begin(), display._bus.writes.end(),
                                  [command](const auto& w) { return w.command == command; });
      assert(it != display._bus.writes.end() && it->bytes.size() == 60000);
      for (unsigned y = 0; y < 600; ++y) for (unsigned x = 0; x < 100; ++x) {
        uint8_t expected = 0xff;
        if (y >= gateOffset && y < gateOffset + 480) {
          unsigned row = y - gateOffset;
          if (reverse) row = 479 - row;
          expected = input[row * 100 + x];
          if (inverted) expected = uint8_t(~expected);
        }
        assert(it->bytes[y * 100 + x] == expected);
      }
    }
    if (snapshot) assert(memcmp(driver._grayBase, bw.data(), bw.size()) == 0);
    display.displayGrayBuffer(turnOff);
    assert(driver._isScreenOn == !turnOff);
    assert(driver._needFullClear && !driver._absoluteInput);
    display.cleanupGrayscaleBuffers(bw.data());
    assert(driver._needFullClear);
    display.displayBuffer(FreeInkDisplay::FAST_REFRESH);
    assert(!driver._needFullClear);
    // Incomplete uploads are canceled without a gray activation and still force a clean.
    assert(display.displayGrayscaleBase(GrayscaleMode::Absolute));
    display.copyGrayscaleLsbBuffers(lsb.data());
    display._bus.clear();
    display.displayGrayBuffer();
    for (const auto& w : display._bus.writes) assert(w.command != 0x12);
    display.cleanupGrayscaleBuffers(bw.data());
    assert(driver._needFullClear && !driver._absoluteInput);
    display.displayGrayscaleBase(FreeInkDisplay::HALF_REFRESH);
    assert(!driver._absoluteInput);
    display.releaseBuffers();
    free(driver._grayBase);
    driver._grayBase = nullptr;
  }
}

static void testUc8179GrayShadeSplit() {
  Uc8179Driver driver;
  freeink::EpdBus bus;
  driver.begin(bus);
  const auto bw = frame(91), lsb = frame(13), msb = frame(29);
  for (auto mode : {GrayscaleMode::Absolute, GrayscaleMode::Overlay}) {
    driver.beginGrayscale(bus, bw.data(), mode, RefreshMode::Half, false);
    driver.copyGrayscaleLsb(bus, lsb.data());
    driver.copyGrayscaleMsb(bus, msb.data());
    bus.clear();
    driver.displayGray(bus, bw.data(), false, nullptr, mode == GrayscaleMode::Absolute);
    const auto light = std::find_if(bus.writes.begin(), bus.writes.end(),
                                    [](const auto& w) { return w.command == 0x22; });
    const auto dark = std::find_if(bus.writes.begin(), bus.writes.end(),
                                   [](const auto& w) { return w.command == 0x23; });
    assert(light != bus.writes.end() && dark != bus.writes.end());
    assert(light->bytes.size() == 42 && dark->bytes.size() == 42);
    assert(light->bytes[0] == 0x20 && light->bytes[1] == 2 && light->bytes[2] == 2);
    auto expected = light->bytes;
    expected[2] = 1;
    expected[3] = 2;
    assert(dark->bytes == expected);
    std::vector<size_t> pllWrites;
    size_t refresh = bus.writes.size();
    for (size_t i = 0; i < bus.writes.size(); ++i) {
      if (bus.writes[i].command == 0x30 && bus.writes[i].bytes.size() == 1) pllWrites.push_back(i);
      if (bus.writes[i].command == 0x12) refresh = i;
    }
    if (mode == GrayscaleMode::Absolute) {
      assert(pllWrites.size() == 2);
      assert(bus.writes[pllWrites[0]].bytes[0] == 0x05 && bus.writes[pllWrites[1]].bytes[0] == 0x06);
      assert(pllWrites[0] < refresh && refresh < pllWrites[1]);
    } else {
      assert(pllWrites.empty());
    }
  }
  free(driver._grayBase);
}

template<class Driver>
static void testDirectSleep() {
  Driver driver;
  FreeInkDisplay display(1, 2, 3, 4, 5, 6);
  display._driver = &driver;
  display.begin();
  const auto bw = frame(11), lsb = frame(17), msb = frame(29);
  std::memcpy(display.getFrameBuffer(), bw.data(), bw.size());
  const auto activations = [&]() {
    return std::count_if(display._bus.writes.begin(), display._bus.writes.end(),
                         [](const auto& w) { return w.command == 0x12; });
  };
  assert(display.grayscaleCapabilities(GrayscaleMode::Direct).base == GrayscaleBase::Combined);
  for (auto fallback : {FreeInkDisplay::HALF_REFRESH, FreeInkDisplay::FAST_REFRESH}) {
    display._bus.clear();
    assert(display.displayGrayscaleBase(GrayscaleMode::Direct, fallback));
    assert(activations() == 0);
    display.copyGrayscaleBuffers(lsb.data(), msb.data());
    assert(activations() == 0);
    display.displayGrayBuffer(false);
    assert(activations() == 1);
    display.cleanupGrayscaleBuffers(bw.data());
    display.displayBuffer(FreeInkDisplay::FAST_REFRESH, false);
    assert(activations() > 1);
  }
  display._bus.clear();
  assert(display.displayGrayscaleBase(GrayscaleMode::Direct));
  display.copyGrayscaleLsbBuffers(lsb.data());
  display.displayGrayBuffer(false);
  assert(activations() == 0);
  display.deepSleep();
  free(driver._grayBase);
}

static void testUc8279X4WaveformSelection() {
  Uc8279X4Driver driver;
  EpdBus bus;
  driver.begin(bus);
  const auto bw = frame(11);
  Bytes lsb(48000, 0), msb(48000, 0);
  const auto bank = [&]() {
    std::vector<Bytes> rows(5);
    for (const auto& w : bus.writes)
      if (w.command >= 0x20 && w.command <= 0x24 && w.bytes.size() == 49) rows[w.command - 0x20] = w.bytes;
    for (const auto& row : rows) assert(row.size() == 49);
    return rows;
  };
  const auto render = [&](GrayscaleMode mode, uint8_t mask, bool factory) {
    std::fill(msb.begin(), msb.end(), mask);
    driver.beginGrayscale(bus, bw.data(), mode, RefreshMode::Half, false);
    driver.copyGrayscaleLsb(bus, lsb.data());
    driver.copyGrayscaleMsb(bus, msb.data());
    bus.clear();
    driver.displayGray(bus, bw.data(), false, nullptr, factory);
    return bank();
  };
  const auto quality = render(GrayscaleMode::Direct, 0, false);
  assert(quality[2] != quality[3]);
  assert(render(GrayscaleMode::Absolute, 0, false) == quality);
  for (uint8_t variant : {0x02, 0x68, 0x69}) {
    BoardConfig::ACTIVE.displayControllerVariant = variant;
    for (uint8_t mask : {0x00, 0x01, 0x03}) {
      const auto text = render(GrayscaleMode::Overlay, mask, false);
      assert(text != quality);
      assert(text[2] == text[3]);
      const uint8_t timing = variant == 0x02 ? 2 : 3;
      for (unsigned t = 0; t < 5; ++t) {
        Bytes expected = {1, 2, timing, 1, 1, 1, 1, 0, 0, 0, 0, 0, 1, 1};
        expected.resize(49, 0);
        if (t == 1) expected[3] |= 0x40;
        if (t == 2 || t == 3) expected[2] |= 0x80;
        if (t == 4) expected[3] |= 0x80;
        assert(text[t] == expected);
      }
    }
    assert(render(GrayscaleMode::Overlay, 0x07, false) == quality);
    assert(render(GrayscaleMode::Overlay, 0, true) == quality);
    // An image pass must not leave the next sparse page on the quality bank.
    assert(render(GrayscaleMode::Overlay, 0x01, false) != quality);
  }
  BoardConfig::ACTIVE.displayControllerVariant = 0x68;
  free(driver._grayBase);
}

static void testStickyAbsolute() {
  BoardConfig::ACTIVE.board = BoardConfig::Board::Sticky;
  auto& driver = static_cast<Ssd1677Driver&>(ssd1677Driver());
  assert(driver._cfg.grayPowerUpFirst);
  assert(driver._cfg.fullSeqOverride == 0xF7);
  assert(driver.grayscaleCapabilities(GrayscaleMode::Absolute).supported());
  FreeInkDisplay display(1, 2, 3, 4, 5, 6);
  display._driver = &driver;
  display.begin();
  const auto bw = frame(33), lsb = frame(14), msb = frame(29);
  std::memcpy(display.getFrameBuffer(), bw.data(), bw.size());
  display._bus.clear();
  assert(display.displayGrayscaleBase(GrayscaleMode::Absolute));
  assert(display._bus.writes.empty());
  display.copyGrayscaleBuffers(lsb.data(), msb.data());
  display.displayGrayBuffer(false);
  assert(lastRegister(display._bus, 0x22) == 0xCC);
  assert(driver._isScreenOn);
  display.cleanupGrayscaleBuffers(bw.data());
  display._bus.clear();
  display.displayBuffer(FreeInkDisplay::FAST_REFRESH);
  assert(lastRegister(display._bus, 0x22) == 0xF7);
  display.releaseBuffers();
}

static void testMetalio() {
  BoardConfig::ACTIVE.board = BoardConfig::Board::MetalioEInk4;
  auto& driver = static_cast<Ssd1677Driver&>(ssd1677Driver());
  EpdBus bus;
  driver.begin(bus);
  assert(lastRegister(bus, 0x3C) == 0x01);
  assert(driver.grayscaleCapabilities(GrayscaleMode::Absolute).supported());
  assert(driver.grayscaleCapabilities().supported());
  const auto bw = frame(42);
  bus.clear();
  // Cold FAST uses the demo's two-partial black pulse, never OTP D7. Both
  // phases run the vendor B/W fast LUT (0xCC activation, no OTP 0xFC).
  driver.display(bus, bw.data(), nullptr, RefreshMode::Fast, false);
  assert(lastRegister(bus, 0x22) == 0xCC);
  unsigned activations = 0;
  std::vector<Bytes> oldPlanes;
  for (const auto& w : bus.writes) {
    if (w.command == 0x20) ++activations;
    if (w.command == 0x26) oldPlanes.push_back(w.bytes);
    if (w.command == 0x22) assert(w.bytes == Bytes({0xCC}));
    if (w.command == 0x32) assert(w.bytes == Bytes(lut_fast_metalio, lut_fast_metalio + 105));
    assert(w.command != 0x1A);
  }
  assert(activations == 2 && bus.waits == 2);
  assert(oldPlanes.size() == 3);
  assert(oldPlanes[0] == Bytes(bw.size(), 0xFF));
  assert(oldPlanes[1] == Bytes(bw.size(), 0x00));
  assert(oldPlanes[2] == bw);
  assert(!driver._customLutActive);
  bus.clear();
  driver.display(bus, bw.data(), nullptr, RefreshMode::Fast, false);
  assert(lastRegister(bus, 0x22) == 0xCC);
  assert(lastRegister(bus, 0x18) == 0x80);
  assert(lastRegister(bus, 0x3C) == 0x80);
  assert(lastRegister(bus, 0x2C) == 0x10);
  for (const auto& w : bus.writes) if (w.command == 0x21) assert(w.bytes == Bytes({0, 0}));
  assert(driver._isScreenOn);
  bus.clear();
  driver.displayGray(bus, bw.data(), false, nullptr, false);
  // X4 external-LUT AA path: LUT + voltage tail written, border parked at 0xC0,
  // absolute 0xCC activation.
  assert(lastRegister(bus, 0x22) == 0xCC);
  assert(lastRegister(bus, 0x3C) == 0xC0);
  bool lutWritten = false;
  for (const auto& w : bus.writes) if (w.command == 0x32) lutWritten = true;
  assert(lutWritten);
  // Exit grayscale the way the facade does so the next FAST isn't promoted.
  driver.cleanupGrayscaleBuffers(bus, bw.data());
  bus.clear();
  driver.displayStart(bus, bw.data(), bw.data(), RefreshMode::Fast, true);
  assert(lastRegister(bus, 0x22) == 0xCF);  // LUT paint, then analog/clock off
  driver.displayFinish(bus, bw.data());
  assert(lastRegister(bus, 0x22) == 0xCF);
  assert(!driver._isScreenOn);
  // Deferred FAST must resync both planes from the frame retained by the facade.
  assert(lastPlane(bus, 0x24) == bw && lastPlane(bus, 0x26) == bw);
  bus.clear();
  driver.displayStart(bus, bw.data(), nullptr, RefreshMode::Half, true);
  assert(bus.waits == 1);  // black pulse done; target waveform still pending
  unsigned pulses = 0;
  for (const auto& w : bus.writes) if (w.command == 0x20) ++pulses;
  assert(pulses == 2);
  assert(lastPlane(bus, 0x26) == Bytes(bw.size(), 0));
  driver.displayFinish(bus, bw.data());
  assert(lastPlane(bus, 0x26) == bw && lastPlane(bus, 0x24) == bw);
  assert(lastRegister(bus, 0x22) == 0xCF);
  driver.deepSleep(bus);
  assert(lastRegister(bus, 0x10) == 0x03);
}

// --- stock-sequence trace -------------------------------------------------
// Hashes every recorded write (command, payload, CS framing) and wait of a
// fixed set of stock operations. run_pro.py builds with the display lab off
// and on and requires identical output, so the lab cannot alter a stock path.
static uint64_t hashBus(const EpdBus& bus) {
  uint64_t h = 1469598103934665603ull;
  const auto mix = [&h](uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) {
      h ^= uint8_t(v >> (8 * i));
      h *= 1099511628211ull;
    }
  };
  for (const auto& w : bus.writes) {
    mix(w.command);
    mix(w.transactions);
    mix(uint32_t(w.bytes.size()));
    for (uint8_t b : w.bytes) {
      h ^= b;
      h *= 1099511628211ull;
    }
  }
  mix(bus.waits);
  return h;
}

static void traceStep(EpdBus& bus, const char* who, const char* op) {
  std::printf("%s %s %016llx\n", who, op, static_cast<unsigned long long>(hashBus(bus)));
  bus.clear();
}

template<class Driver>
static void traceDriver(const char* who) {
  Driver d;
  EpdBus bus;
  const auto a = frame(5), b = frame(71), lsb = frame(13), msb = frame(29);
  d.begin(bus);
  traceStep(bus, who, "begin");
  d.display(bus, a.data(), nullptr, RefreshMode::Full, false);
  traceStep(bus, who, "full");
  d.display(bus, b.data(), nullptr, RefreshMode::Fast, false);
  traceStep(bus, who, "fast");
  d.display(bus, a.data(), b.data(), RefreshMode::Half, false);
  traceStep(bus, who, "half");
  d.displayStart(bus, b.data(), a.data(), RefreshMode::Fast, false);
  d.displayFinish(bus, b.data());
  traceStep(bus, who, "async");
  for (auto mode : {GrayscaleMode::Overlay, GrayscaleMode::Absolute, GrayscaleMode::Direct}) {
    if (!d.grayscaleCapabilities(mode).supported()) continue;
    d.beginGrayscale(bus, a.data(), mode, RefreshMode::Fast, false);
    d.copyGrayscaleLsb(bus, lsb.data());
    d.copyGrayscaleMsb(bus, msb.data());
    d.displayGray(bus, msb.data(), false, nullptr, mode != GrayscaleMode::Overlay);
    d.cleanupGrayscaleBuffers(bus, a.data());
    d.display(bus, b.data(), nullptr, RefreshMode::Fast, false);
    traceStep(bus, who, "gray");
  }
  d.displayGray(bus, a.data(), false, lut_grayscale, false);
  d.display(bus, a.data(), nullptr, RefreshMode::Fast, true);
  traceStep(bus, who, "custom-lut");
  d.display(bus, b.data(), nullptr, RefreshMode::Full, true);
  d.display(bus, a.data(), nullptr, RefreshMode::Half, true);
  traceStep(bus, who, "power-off");
  d.deepSleep(bus);
  traceStep(bus, who, "sleep");
  if constexpr (!std::is_same<Driver, Ssd1677Driver>::value) free(d._grayBase);
}

template<class Driver>
static void traceFacade(const char* who) {
  Driver driver;
  FreeInkDisplay display(1, 2, 3, 4, 5, 6);
  display._driver = &driver;
  display.begin();
  const auto a = frame(8), lsb = frame(13), msb = frame(29);
  std::memcpy(display.getFrameBuffer(), a.data(), a.size());
  traceStep(display._bus, who, "begin");
  display.displayBuffer(FreeInkDisplay::FULL_REFRESH);
  display.displayBufferAsync(FreeInkDisplay::FAST_REFRESH);
  display.waitRefreshComplete();
  traceStep(display._bus, who, "refresh");
  if (display.displayGrayscaleBase(GrayscaleMode::Absolute)) {
    display.copyGrayscaleBuffers(lsb.data(), msb.data());
    display.displayGrayBuffer(false);
    display.cleanupGrayscaleBuffers(a.data());
    traceStep(display._bus, who, "gray");
  }
  display.setInverted(true);
  display.displayBuffer(FreeInkDisplay::FAST_REFRESH);
  display.setInverted(false);
  display.deepSleep();
  traceStep(display._bus, who, "invert-sleep");
  display.releaseBuffers();
  if constexpr (!std::is_same<Driver, Ssd1677Driver>::value) free(driver._grayBase);
}

static void traceStock() {
  traceDriver<Ssd1677Driver>("ssd1677");
  traceDriver<Uc8179Driver>("uc8179");
  for (uint8_t variant : {0x02, 0x68}) {
    BoardConfig::ACTIVE.displayControllerVariant = variant;
    traceDriver<Uc8279X4Driver>(variant == 0x02 ? "uc8279x4-02" : "uc8279x4-68");
  }
  traceFacade<Ssd1677Driver>("facade-ssd1677");
  traceFacade<Uc8179Driver>("facade-uc8179");
  traceFacade<Uc8279X4Driver>("facade-uc8279x4");
}

#if FREEINK_CAP_DISPLAY_LAB
// --- display lab ------------------------------------------------------------
static std::vector<uint8_t> commands(const EpdBus& bus) {
  std::vector<uint8_t> out;
  for (const auto& w : bus.writes) out.push_back(w.command);
  return out;
}

static const Bytes& written(const EpdBus& bus, size_t index) { return bus.writes.at(index).bytes; }

// A framebuffer-order plane as the UltraChip streamPlane lays it over 600 gates.
static Bytes ucPlane(const Bytes& in, bool reverse, unsigned gateOffset) {
  Bytes out(60000, 0xff);
  for (unsigned y = 0; y < 480; ++y) {
    const unsigned row = reverse ? 479 - y : y;
    std::copy(in.begin() + row * 100, in.begin() + (row + 1) * 100, out.begin() + (gateOffset + y) * 100);
  }
  return out;
}

static Bytes labTables() {
  Bytes t(5 * 43);
  for (unsigned r = 0; r < 5; ++r) {
    t[r * 43] = uint8_t(0x20 + r);
    for (unsigned i = 1; i < 43; ++i) t[r * 43 + i] = uint8_t(r * 50 + i);
  }
  return t;
}

static void assertNone(const EpdBus& bus, std::initializer_list<uint8_t> forbidden) {
  for (const auto& w : bus.writes)
    for (uint8_t c : forbidden) assert(w.command != c);
}

// UltraChip: PWR (0x01), POF (0x02), VDCS (0x82) and the OTP program/active
// program commands (0xA0/0xA1) never appear in a lab session.
static const std::initializer_list<uint8_t> kUcForbidden = {0x01, 0x02, 0x82, 0xA0, 0xA1};

struct UcLabExpect {
  bool reverse;
  unsigned gateOffset;
  Bytes psrLab, psrOtp, cdiLab, cdiStock;
  uint8_t pllStock;
  size_t tableBytes;
  bool psrAfterPon;
};

template<class Driver>
static void testLabUltraChip(const UcLabExpect& x) {
  Driver d;
  EpdBus bus;
  d.begin(bus);
  const auto p1 = frame(41), p2 = frame(67), tables = labTables();
  uint32_t busyUs = 123;
  bus.clear();
  assert(d.labSupported());
  // No waveform yet: PSR REG is still 0, so a scan would run OTP; refused.
  assert(!d.labScan(bus, p1.data(), p2.data(), busyUs) && busyUs == 0);
  // Malformed uploads are refused before any bus traffic.
  Bytes swapped = tables;
  std::swap(swapped[2 * 43], swapped[3 * 43]);
  assert(!d.labUploadWaveform(bus, tables.data(), tables.size() - 1));
  assert(!d.labUploadWaveform(bus, tables.data(), tables.size() + 1));
  assert(!d.labUploadWaveform(bus, swapped.data(), swapped.size()));
  assert(!d.labUploadWaveform(bus, nullptr, tables.size()));
  assert(!d.labScan(bus, p1.data(), p2.data(), busyUs));
  assert(bus.writes.empty() && bus.waits == 0);
  // labEnd outside a session touches nothing.
  d.labEnd(bus);
  assert(bus.writes.empty());

  assert(d.labUploadWaveform(bus, tables.data(), tables.size()));
  assert((commands(bus) == std::vector<uint8_t>{0x00, 0x20, 0x21, 0x22, 0x23, 0x24, 0x50}));
  assert(written(bus, 0) == x.psrLab);
  for (unsigned r = 0; r < 5; ++r) {
    Bytes expected(tables.begin() + r * 43 + 1, tables.begin() + (r + 1) * 43);
    expected.resize(x.tableBytes, 0);
    assert(written(bus, 1 + r) == expected);
  }
  assert(written(bus, 6) == x.cdiLab);
  assert(bus.waits == 1);
  assertNone(bus, kUcForbidden);

  bus.clear();
  assert(d.labSetFrameRate(bus, 0x09));
  assert((commands(bus) == std::vector<uint8_t>{0x30}) && written(bus, 0) == Bytes{0x09});

  for (unsigned scan = 0; scan < 2; ++scan) {
    bus.clear();
    busyUs = 0;
    assert(d.labScan(bus, p1.data(), p2.data(), busyUs));
    assert(busyUs > 0);
    std::vector<uint8_t> expected = {0x10, 0x13};
    if (scan == 0) expected.push_back(0x04);  // PON once; power stays on after
    if (x.psrAfterPon) expected.push_back(0x00);
    expected.push_back(0x12);
    assert(commands(bus) == expected);
    // Raw planes: no inversion, no fold; only the driver's gate mapping.
    assert(written(bus, 0) == ucPlane(p1, x.reverse, x.gateOffset));
    assert(written(bus, 1) == ucPlane(p2, x.reverse, x.gateOffset));
    assert(bus.writes[0].transactions == 1 && bus.writes[1].transactions == 1);
    if (x.psrAfterPon) assert(written(bus, expected.size() - 2) == x.psrLab);
    assert(bus.waits == (scan == 0 ? 3u : 2u));  // ready, [PON], DRF
    assert(d._isScreenOn);
    assertNone(bus, kUcForbidden);
  }

  bus.clear();
  d.labEnd(bus);
  assert((commands(bus) == std::vector<uint8_t>{0x00, 0x50, 0x30, 0xE5}));
  assert(written(bus, 0) == x.psrOtp && written(bus, 1) == x.cdiStock);
  assert(written(bus, 2) == Bytes{x.pllStock} && written(bus, 3) == Bytes{0x1E});
  assert(d._needFullClear && !d._oldPlaneValid);
  assertNone(bus, kUcForbidden);
  bus.clear();
  d.labEnd(bus);
  assert(bus.writes.empty());
  // The session's waveform is gone with it.
  assert(!d.labScan(bus, p1.data(), p2.data(), busyUs) && bus.writes.empty());

  // The next ordinary Fast is the full GC from a white OLD plane, not a DU.
  d.display(bus, p1.data(), nullptr, RefreshMode::Fast, false);
  assert(lastRegister(bus, 0xE5) == 0x1E);
  assert(lastPlane(bus, 0x10) == ucPlane(p1, x.reverse, x.gateOffset));  // post-refresh sync
  bool whiteSeed = false;
  for (const auto& w : bus.writes) {
    assert(w.command != 0x91);
    if (w.command == 0x10 && w.bytes == Bytes(60000, 0xff)) whiteSeed = true;
  }
  assert(whiteSeed);
  free(d._grayBase);
}

static void testLabUc8179DirectGrayExit() {
  // A lab session entered while direct gray owns PWR/VDCS resets to the stock
  // B/W power setup first and writes no voltage register itself.
  Uc8179Driver d;
  EpdBus bus;
  d.begin(bus);
  const auto bw = frame(3), tables = labTables();
  d.beginGrayscale(bus, bw.data(), GrayscaleMode::Direct, RefreshMode::Half, false);
  assert(d._directGrayConfigured);
  bus.clear();
  assert(d.labUploadWaveform(bus, tables.data(), tables.size()));
  assert(!d._directGrayConfigured);
  assertNone(bus, kUcForbidden);
  const auto c = commands(bus);
  assert(c.front() == 0x00 && c[1] == 0x61);  // initController after reset
  const std::vector<uint8_t> upload = {0x00, 0x20, 0x21, 0x22, 0x23, 0x24, 0x50};
  assert(std::vector<uint8_t>(c.end() - 7, c.end()) == upload);
  d.labEnd(bus);
  free(d._grayBase);
}

static void testLabSsd1677() {
  Ssd1677Driver d;
  EpdBus bus;
  d.begin(bus);
  const auto bw = frame(9), p1 = frame(41), p2 = frame(67);
  d.display(bus, bw.data(), nullptr, RefreshMode::Fast, false);  // consume the boot clean
  bus.clear();
  Bytes lut(112);
  for (size_t i = 0; i < lut.size(); ++i) lut[i] = uint8_t(i * 7 + 3);
  uint32_t busyUs = 5;
  assert(d.labSupported());
  assert(!d.labSetFrameRate(bus, 0x22));  // rate lives in the LUT bytes
  assert(!d.labScan(bus, p1.data(), p2.data(), busyUs) && busyUs == 0);
  assert(!d.labUploadWaveform(bus, lut.data(), 110));
  assert(!d.labUploadWaveform(bus, nullptr, 112));
  assert(bus.writes.empty());
  d.labEnd(bus);
  assert(!d._needsGrayClear);

  assert(d.labUploadWaveform(bus, lut.data(), lut.size()));
  assert((commands(bus) == std::vector<uint8_t>{0x32, 0x03, 0x04, 0x2C, 0x3C}));
  assert(written(bus, 0) == Bytes(lut.begin(), lut.begin() + 105));
  assert(written(bus, 1) == Bytes{lut[105]});
  assert(written(bus, 2) == Bytes(lut.begin() + 106, lut.begin() + 109));
  assert(written(bus, 3) == Bytes{lut[109]});
  assert(written(bus, 4) == Bytes{0xC0});  // borderWaveformGray
  assert(!d._customLutActive);  // ordinary refreshes keep their own LUT selection

  for (unsigned scan = 0; scan < 2; ++scan) {
    bus.clear();
    assert(d.labScan(bus, p1.data(), p2.data(), busyUs) && busyUs > 0);
    assert((commands(bus) ==
            std::vector<uint8_t>{0x11, 0x44, 0x45, 0x4E, 0x4F, 0x24, 0x26, 0x21, 0x22, 0x20}));
    assert(written(bus, 0) == Bytes{0x01});
    assert((written(bus, 1) == Bytes{0x00, 0x00, 0x1F, 0x03}));
    assert((written(bus, 2) == Bytes{0xDF, 0x01, 0x00, 0x00}));
    assert((written(bus, 3) == Bytes{0x00, 0x00}));
    assert((written(bus, 4) == Bytes{0xDF, 0x01}));
    assert(written(bus, 5) == p1 && written(bus, 6) == p2);  // raw, framebuffer order
    assert(written(bus, 7) == Bytes{0x00} && written(bus, 8) == Bytes{0xCC} && written(bus, 9).empty());
    assert(bus.waits == 1 && d._isScreenOn);
    assertNone(bus, {0x30, 0x36, 0x39, 0x32, 0x03, 0x04, 0x2C});
  }
  bus.clear();
  d.labEnd(bus);
  assert(bus.writes.empty() && d._needsGrayClear);
  assert(!d.labScan(bus, p1.data(), p2.data(), busyUs));
  // The next Fast is promoted to the clean HALF (stock 0xD7), both planes rewritten.
  d.display(bus, bw.data(), nullptr, RefreshMode::Fast, false);
  assert(lastRegister(bus, 0x22) == 0xD7);
  assert(lastPlane(bus, 0x24) == bw && lastPlane(bus, 0x26) == bw);
}

static void testLabFacade() {
  {
    BwOnlyDriver bw;
    FreeInkDisplay display(1, 2, 3, 4, 5, 6);
    display._driver = &bw;
    const auto t = labTables(), p = frame(1);
    uint32_t busyUs = 9;
    assert(!display.labSupported());
    assert(!display.labUploadWaveform(t.data(), t.size()));
    assert(!display.labSetFrameRate(1));
    assert(!display.labScan(p.data(), p.data(), busyUs) && busyUs == 0);
    display.labEnd();
    assert(display._bus.writes.empty());
  }
  Uc8179Driver driver;
  FreeInkDisplay display(1, 2, 3, 4, 5, 6);
  display._driver = &driver;
  display.begin();
  const auto fb = frame(19), p1 = frame(41), p2 = frame(67), tables = labTables();
  std::memcpy(display.getFrameBuffer(), fb.data(), fb.size());
  assert(display.labSupported());
  uint32_t busyUs = 0;
  // Inverted output: the planes would go out raw, so every lab call refuses.
  display.setInverted(true);
  display._bus.clear();
  assert(!display.labUploadWaveform(tables.data(), tables.size()));
  assert(!display.labSetFrameRate(0x09));
  assert(!display.labScan(p1.data(), p2.data(), busyUs));
  display.labEnd();
  assert(display._bus.writes.empty());
  display.setInverted(false);
  display.displayBuffer(FreeInkDisplay::FULL_REFRESH);

  // A pending async refresh is finished before the lab touches the bus.
  display.displayBufferAsync(FreeInkDisplay::FAST_REFRESH);
  assert(display.isRefreshPending());
  display._bus.clear();
  assert(display.labUploadWaveform(tables.data(), tables.size()));
  assert(!display.isRefreshPending());
  const auto c = commands(display._bus);
  assert(std::find(c.begin(), c.end(), 0x10) < std::find(c.begin(), c.end(), 0x20));  // DTM1 sync first
  assert(display.labSetFrameRate(0x09));
  assert(display.labScan(p1.data(), p2.data(), busyUs) && busyUs > 0);
  assert(!display._shadowValid && !display._redRamSynced);
  assert(lastPlane(display._bus, 0x10) == ucPlane(p1, true, 0));
  display._bus.clear();
  display.labEnd();
  assert((commands(display._bus) == std::vector<uint8_t>{0x00, 0x50, 0x30, 0xE5}));
  display._bus.clear();
  display.displayBuffer(FreeInkDisplay::FAST_REFRESH);
  assert(lastRegister(display._bus, 0xE5) == 0x1E);  // full resync, not the DU
  display.releaseBuffers();
  free(driver._grayBase);

  // SSD1677 through the facade: an open absolute gray pass is cancelled and
  // the planes reach BW/RED RAM unmodified.
  Ssd1677Driver ssd;
  FreeInkDisplay x4(1, 2, 3, 4, 5, 6);
  x4._driver = &ssd;
  x4.begin();
  assert(x4.displayGrayscaleBase(GrayscaleMode::Absolute));
  Bytes lut(112, 0);
  assert(x4.labUploadWaveform(lut.data(), lut.size()));
  assert(x4._grayscaleMode == GrayscaleMode::Overlay);
  assert(!x4.labSetFrameRate(0x22));
  assert(x4.labScan(p1.data(), p2.data(), busyUs));
  assert(lastPlane(x4._bus, 0x24) == p1 && lastPlane(x4._bus, 0x26) == p2);
  x4.labEnd();
  assert(ssd._needsGrayClear);
  x4.releaseBuffers();
}

static void testDisplayLab() {
  testLabSsd1677();
  testLabUltraChip<Uc8179Driver>({true, 0, {0x3F, 0x0A}, {0x1F, 0x0A}, {0x29, 0x07}, {0xA9, 0x07}, 0x06, 42, false});
  for (uint8_t variant : {0x02, 0x67, 0x68}) {
    BoardConfig::ACTIVE.displayControllerVariant = variant;
    testLabUltraChip<Uc8279X4Driver>({false, 120, {0x37, 0x4D}, {0x17, 0x4D}, {0x97}, {0x97}, 0x0E, 49, true});
  }
  BoardConfig::ACTIVE.displayControllerVariant = 0x68;
  testLabUc8179DirectGrayExit();
  testLabFacade();
  std::puts("Display lab uploads, frame rate, raw-plane scans, refusals and labEnd resync passed");
}
#endif

int main(int argc, char** argv) {
  if (argc > 1 && std::strcmp(argv[1], "trace") == 0) {
    traceStock();
    return 0;
  }
  if (argc > 1 && std::strcmp(argv[1], "metalio") == 0) {
    testMetalio();
    std::puts("Metalio refresh, temperature, grayscale fallback and async power-down passed");
    return 0;
  }
  if (argc > 1) {
    testStickyAbsolute();
    std::puts("Sticky absolute capability, activation, power-down and B/W recovery passed");
    return 0;
  }
  testUc8179GrayShadeSplit();
  testUc8279X4WaveformSelection();
  testDirectSleep<Uc8179Driver>();
  testDirectSleep<Uc8279X4Driver>();
  testUltraChipAbsolute<Uc8179Driver>(true, 0, false);
  for (uint8_t variant : {0x02, 0x68, 0x69}) {
    BoardConfig::ACTIVE.displayControllerVariant = variant;
    testUltraChipAbsolute<Uc8279X4Driver>(false, 120, true);
  }
  testAbsolutePipeline();
  testCapabilities();
  testStream<Uc8179Driver>(true,0);
  testStream<Uc8279X4Driver>(false,120);
  testSsd();
  testAsyncFrame<Uc8179Driver>();
  testAsyncFrame<Uc8279X4Driver>();
#if FREEINK_CAP_DISPLAY_LAB
  testDisplayLab();
#endif
  std::puts("Pro plane bytes, transaction counts, clean refreshes, power state and async frame ownership passed");
}
