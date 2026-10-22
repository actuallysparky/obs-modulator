#include "lfo.hpp"
#include <cstdio>
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
  std::printf("%d core checks passed\n",checks);
}
