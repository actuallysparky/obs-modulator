// SPDX-FileCopyrightText: 2026 Sparky and OBS Modulator contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "sample-history.hpp"
#include <cstdio>
#include <cstdlib>
#include <limits>
static int checks=0;
#define CHECK(x) do {++checks;if(!(x)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
int main() {
  modulator::SampleHistory h;CHECK(h.count==0);CHECK(h.append(1,0));
  CHECK(!h.append(2,.001));CHECK(h.count==1 && h.at(0).value==1);
  CHECK(h.append(3,.1));CHECK(h.at(1).value==3);
  for(int i=2;i<10000;++i) {
    const double now=i*.067;CHECK(h.append(i,now));
    CHECK(h.count<=150 && h.count>0);CHECK(now-h.at(0).time<=10);
    CHECK(h.at(h.count-1).value==i);
  }
  CHECK(h.append(42,1000));CHECK(h.count==1 && h.at(0).value==42);
  CHECK(h.append(43,900));CHECK(h.count==1 && h.at(0).time==900);
  CHECK(!h.append(std::numeric_limits<double>::quiet_NaN(),901));
  CHECK(!h.append(44,std::numeric_limits<double>::infinity()));CHECK(h.count==1);
  h.clear();CHECK(h.count==0);
  std::printf("%d bounded history checks passed\n",checks);
}
