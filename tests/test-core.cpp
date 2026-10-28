// SPDX-FileCopyrightText: 2026 Sparky and OBS Modulator contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "lfo.hpp"
#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <limits>
static int checks;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); std::exit(1); } } while(0)
int main()
{
  using namespace modulator;
  CHECK(normalized(Wave::Sine,0)==0);
  CHECK(normalized(Wave::Sine,.5)==1);
  CHECK(normalized(Wave::Triangle,.25)==.5);
  CHECK(normalized(Wave::RisingSaw,.25)==.25);
  CHECK(normalized(Wave::FallingSaw,.25)==.75);
  CHECK(normalized(Wave::Square,.49)==0);
  CHECK(normalized(Wave::Square,.5)==1);
  CHECK(normalized(Wave::Sine,1)==0);
  for(int wave=0;wave<5;++wave) for(int i=0;i<=1000;++i) {
    double v=mapped(static_cast<Wave>(wave),i*.013,4,-3,17,false,-3,17);
    CHECK(v>=-3 && v<=17);
    CHECK(same_value(v,mapped(static_cast<Wave>(wave),i*.013+400,4,-3,17,false,-3,17),false));
  }
  CHECK(mapped(Wave::RisingSaw,.51,1,0,3,true,0,3)==2);
  CHECK(mapped(Wave::Square,.6,1,-10,10,false,-2,2)==2);
  CHECK(std::isnan(mapped(Wave::Sine,1,0,0,1,false,0,1)));
  CHECK(std::isnan(mapped(Wave::Sine,1,4,2,1,false,0,3)));
  CHECK(!same_value(1,1.0001,false));
  CHECK(same_value(1,1+1e-12,false));
  CHECK(!same_value(std::numeric_limits<double>::quiet_NaN(),1,false));
  // Glider is deterministic value noise, not a repeating period or a step.
  constexpr uint64_t seed = UINT64_C(0x123456789abcdef0);
  bool different_seed=false, nonperiodic=false;
  for(int i=0;i<2000;++i) {
    const double t=i*.037;
    const double v=normalized(Wave::Glider,t,seed);
    CHECK(v>=0 && v<=1);
    CHECK(v==normalized(Wave::Glider,t,seed));
    different_seed |= v!=normalized(Wave::Glider,t,seed+1);
    nonperiodic |= v!=normalized(Wave::Glider,t+1,seed);
    CHECK(mapped(Wave::Glider,t,4,-3,17,false,-3,17,seed)>=-3);
    CHECK(mapped(Wave::Glider,t,4,-3,17,false,-3,17,seed)<=17);
  }
  CHECK(different_seed);CHECK(nonperiodic);
  for(int i=-5;i<100;++i) {
    const double a=normalized(Wave::Glider,i,seed), b=normalized(Wave::Glider,i+1,seed);
    double previous=a;
    for(int j=1;j<=100;++j) {
      const double v=normalized(Wave::Glider,i+j*.01,seed);
      CHECK(v>=std::min(a,b)-1e-14 && v<=std::max(a,b)+1e-14);
      CHECK(b>=a ? v>=previous-1e-14 : v<=previous+1e-14);previous=v;
    }
    constexpr double epsilon=1e-4;
    const double left=normalized(Wave::Glider,i-epsilon,seed), right=normalized(Wave::Glider,i+epsilon,seed);
    CHECK(std::abs(left-a)<1e-9 && std::abs(right-a)<1e-9);
    CHECK(std::abs((a-left)/epsilon)<1e-5 && std::abs((right-a)/epsilon)<1e-5);
    CHECK(std::abs((right-2*a+left)/(epsilon*epsilon))<.03);
  }
  CHECK(normalized(Wave::Glider,std::numeric_limits<double>::infinity(),seed)==0);
  CHECK(normalized(Wave::Glider,0x1p63,seed)==0);
  CHECK(normalized(Wave::Glider,-0x1p64,seed)==0);
  CHECK(std::isfinite(normalized(Wave::Glider,-0x1p63,seed)));

  // Long cycles stay smooth at video-frame intervals, including knot crossings.
  for(double period : {4.0, 60.0, 86400.0}) {
    const double step=1.0/60.0;
    for(int knot_index=0;knot_index<20;++knot_index) {
      for(int frame=-2;frame<=2;++frame) {
        const double time=knot_index*period+frame*step;
        const double a=normalized(Wave::Glider,time/period,seed);
        const double b=normalized(Wave::Glider,(time+step)/period,seed);
        CHECK(std::abs(b-a)<=1.876*step/period+1e-12);
      }
    }
  }
  // Fade from the real value, even outside LFO endpoints, within property limits.
  CHECK(mapped(Wave::Sine,0,4,.2,.4,false,0,1,seed,true,.83)==.83);
  CHECK(mapped(Wave::Sine,.0001,4,.2,.4,false,0,1,seed,true,.83)>.829999999);
  CHECK(same_value(mapped(Wave::Square,2,4,0,1,false,0,1,seed,true,.8),.9,false));
  CHECK(mapped(Wave::Square,1,4,0,10,true,0,10,seed,true,7)==6);
  CHECK(mapped(Wave::Sine,0,4,0,10,false,0,10,seed,true,20)==10);
  for(int wave=0;wave<6;++wave) {
    for(int i=0;i<100;++i) {
      const double time=4+i*.027;
      const double plain=mapped(static_cast<Wave>(wave),time,4,.2,.4,false,0,1,seed);
      CHECK(same_value(plain,mapped(static_cast<Wave>(wave),time,4,.2,.4,false,0,1,seed,true,.83),false));
      CHECK(plain==mapped(static_cast<Wave>(wave),time,4,.2,.4,false,0,1,seed,false,.83));
      CHECK(mapped(static_cast<Wave>(wave),time,4,0,10,true,0,10,seed,true,7)==std::round(mapped(static_cast<Wave>(wave),time,4,0,10,false,0,10,seed,true,7)));
    }
    CHECK(mapped(static_cast<Wave>(wave),0,4,.2,.4,false,0,1,seed,true,.83)==.83);
  }
  CHECK(std::isnan(mapped(Wave::Sine,1,4,0,1,false,0,1,seed,true,std::numeric_limits<double>::quiet_NaN())));
  // A stop returns continuously from the last output to the activation snapshot.
  CHECK(eased_return(0,4,.3,.83,false,0,1)==.3);
  CHECK(eased_return(4,4,.3,.83,false,0,1)==.83);
  CHECK(eased_return(40,4,.3,.83,false,0,1)==.83);
  CHECK(same_value(eased_return(2,4,.3,.83,false,0,1),.565,false));
  CHECK(eased_return(1,4,7,0,true,0,10)==6);
  CHECK(eased_return(4,4,0,20,false,0,10)==10);
  CHECK(eased_return(4,4,10,-20,true,0,10)==0);
  for(int i=0;i<1000;++i) {
    const double t=i*.004, a=eased_return(t,4,.3,.83,false,0,1);
    const double b=eased_return(t+.004,4,.3,.83,false,0,1);
    CHECK(a>=.3 && a<=.83);CHECK(b>=a);
    CHECK(same_value(eased_return(t,4,.83,.3,false,0,1),1.13-a,false));
    CHECK(eased_return(t,4,3,8,true,0,10)==std::round(eased_return(t,4,3,8,false,0,10)));
  }
  CHECK(eased_return(.0001,4,.3,.83,false,0,1)<.300000001);
  CHECK(eased_return(3.9999,4,.3,.83,false,0,1)>.829999999);
  CHECK(std::isnan(eased_return(0,0,0,1,false,0,1)));
  CHECK(std::isnan(eased_return(0,4,0,1,false,2,1)));
  CHECK(std::isnan(eased_return(0,4,0,std::numeric_limits<double>::infinity(),false,0,1)));
  // Finite endpoints deliberately differ from periodic wrapping for saw/square.
  for(int wave=0;wave<wave_count;++wave)for(bool ease:{false,true}) {
    auto w=static_cast<Wave>(wave);
    const double end=shot_mapped(w,4,4,-3,17,false,-20,20,seed,ease,7,1);
    const bool high=wave==2 || wave==4 || (wave>=6 && wave<=8);
    CHECK(end==(wave==5 ? -3+20*random_value(seed,1) : high ? 17:-3));
    CHECK(shot_mapped(w,400,4,-3,17,false,-20,20,seed,ease,7,1)==end);
    if(ease || wave==5)CHECK(shot_mapped(w,0,4,-3,17,false,-20,20,seed,ease,7,1)==7);
    for(int i=0;i<=1000;++i) {
      const double v=shot_mapped(w,i*.004,4,-3,17,false,-20,20,seed,ease,7,1);
      CHECK(v>=-3 && v<=17);
      CHECK(shot_mapped(w,i*.004,4,-3,17,true,-20,20,seed,ease,7,1)==std::round(v));
    }
  }
  CHECK(shot_mapped(Wave::Sine,2,4,0,100,false,0,100,0,true,25,0)==62.5);
  CHECK(shot_mapped(Wave::Square,4,4,0,1,false,0,1,0,false,0,0)==1);
  CHECK(normalized(Wave::Square,1)==0);
  for(int wave=6;wave<wave_count;++wave) {
    auto w=static_cast<Wave>(wave);double previous=wave<9 ? -3:17;
    for(int i=0;i<=1000;++i) {
      const double v=shot_mapped(w,i*.004,4,-3,17,false,-3,17,0,false,0,0);
      CHECK(wave<9 ? v>=previous-1e-12 : v<=previous+1e-12);previous=v;
      if(i<1000)CHECK(same_value(normalized(w,i*.001),normalized(w,3+i*.001),false));
    }
    CHECK(normalized(w,1)==(wave<9 ? 0:1));
  }
  CHECK(normalized(Wave::ExponentialRising,.5)<.2);
  CHECK(normalized(Wave::LogarithmicRising,.5)>.8);
  CHECK(normalized(Wave::SCurveRising,.5)==.5);
  CHECK(normalized(Wave::ExponentialFalling,.5)<.2);
  for(int i=0;i<100;++i) {
    CHECK(shot_mapped(Wave::Glider,i*.04,4,0,1,false,0,1,seed,false,.7,1)==
      shot_mapped(Wave::Glider,i*.04,4,0,1,false,0,1,seed,true,.7,1));
  }
  CHECK(random_value(seed,1)!=random_value(seed,2));
  CHECK(std::isnan(shot_mapped(Wave::Sine,0,0,0,1,false,0,1,0,false,0,1)));
  std::printf("%d core checks passed\n",checks);
}
