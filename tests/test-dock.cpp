// SPDX-FileCopyrightText: 2026 Sparky and OBS Modulator contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "dock.hpp"
#include <QApplication>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QMainWindow>
#include <QTableView>
#include <QThread>
#include <QSignalBlocker>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <functional>

static int checks=0;
#define CHECK(x) do {++checks;if(!(x)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
struct Counters { int samples=0, clears=0, toggles=0; };
class Fixture final : public modulator::MonitorFeed {
public:
  std::shared_ptr<Counters> counts=std::make_shared<Counters>();
  std::vector<modulator::MonitorRow> rows;
  std::function<void()> on_sample;
  std::vector<modulator::MonitorRow> sample() override {
    ++counts->samples;
    if(on_sample){auto action=std::move(on_sample);on_sample={};action();}
    return rows;
  }
  void clear() override {++counts->clears;}
  bool set_enabled(const std::string &id,bool enabled) override {
    for(auto &r:rows)if(r.id==id){r.enabled=enabled;++counts->toggles;return true;}
    return false;
  }
};
static void run(int ms) {
  QElapsedTimer time;time.start();
  while(time.elapsed()<ms){QCoreApplication::processEvents();QThread::msleep(1);}
}
int main(int argc,char **argv) {
  QApplication app(argc,argv);QMainWindow main;main.resize(700,500);
  auto source=std::make_unique<Fixture>();auto *fixture=source.get();auto counts=fixture->counts;
  modulator::MonitorRow r;r.id="fixture";r.name="Ship speed";r.source="Astral Cartographer";
  r.target="Orbital Cartography";r.property="Ship speed";r.available=true;r.value=.17;r.low=.03;r.high=.28;
  r.state=modulator::MonitorState::Running;r.waveform=5;r.enabled=true;fixture->rows.push_back(r);
  auto *panel=new modulator::MonitorPanel(std::move(source),&main);
  auto *dock=new QDockWidget("Modulator Monitor",&main);dock->setWidget(panel);main.addDockWidget(Qt::RightDockWidgetArea,dock);
  QObject::connect(dock,&QDockWidget::visibilityChanged,panel,[panel](bool visible){panel->set_exposed(visible);});
  dock->hide();main.show();run(160);CHECK(counts->samples==0 && !panel->monitoring() && panel->view()==nullptr);
  dock->show();dock->hide();run(100);CHECK(counts->samples==0 && panel->view()==nullptr);
  {QSignalBlocker block(dock);dock->show();} // OBS's actual Docks-menu implementation
  run(220);
  if(!panel->monitoring() || counts->samples<2)
    std::fprintf(stderr,"Dock show diagnostic: active=%d panel_visible=%d dock_visible=%d main_visible=%d built=%d samples=%d ticks=%llu history=%llu\n",
      panel->monitoring(),panel->isVisible(),dock->isVisible(),main.isVisible(),panel->view()!=nullptr,counts->samples,
      static_cast<unsigned long long>(panel->timer_callbacks),static_cast<unsigned long long>(panel->history_appends));
  CHECK(panel->monitoring() && counts->samples>=2);CHECK(panel->history_appends>=2);
  auto *model=panel->view()->model();CHECK(model->rowCount()==1);CHECK(model->columnCount()==3);
  CHECK(model->data(model->index(0,2),Qt::DisplayRole).toString()=="0.17");
  CHECK(model->setData(model->index(0,0),Qt::Unchecked,Qt::CheckStateRole));CHECK(counts->toggles==1 && !fixture->rows[0].enabled);
  fixture->rows[0].state=modulator::MonitorState::EasingOut;fixture->rows[0].value=.19;run(100);
  CHECK(model->data(model->index(0,1),Qt::DisplayRole).toString().contains("Easing out"));
  CHECK(model->data(model->index(0,0),Qt::CheckStateRole).toInt()==Qt::Unchecked);
  {QSignalBlocker block(dock);dock->hide();}
  run(40);const int stopped=counts->samples;const auto history=panel->history_appends;
  const auto stopped_ticks=panel->timer_callbacks;
  const auto stopped_paints=panel->test_paints();
  run(240);CHECK(counts->samples==stopped && panel->history_appends==history && !panel->monitoring());
  CHECK(panel->timer_callbacks==stopped_ticks);
  CHECK(panel->test_paints()==stopped_paints);
  CHECK(model->rowCount()==0);CHECK(!model->setData(model->index(0,0),Qt::Checked,Qt::CheckStateRole));
  fixture->rows[0].value=.21;dock->show();run(100);CHECK(panel->monitoring());
  CHECK(model->data(model->index(0,2),Qt::DisplayRole).toString()=="0.21");
  fixture->on_sample=[panel]{panel->suspend(true);};panel->test_refresh();
  CHECK(!panel->monitoring() && model->rowCount()==0);panel->suspend(false);run(100);
  panel->suspend(true);const int suspended=counts->samples;run(200);CHECK(counts->samples==suspended);
  fixture->rows.clear();panel->suspend(false);run(100);CHECK(model->rowCount()==0);
  fixture->rows.push_back(r);run(100);CHECK(model->rowCount()==1);
  main.showMinimized();run(80);const int minimized=counts->samples;run(200);CHECK(!panel->monitoring() && counts->samples==minimized);
  main.showNormal();run(120);CHECK(panel->monitoring() && counts->samples>minimized);
  panel->hide();run(60);const int transient=counts->samples;run(120);CHECK(counts->samples==transient);
  panel->show();run(100);CHECK(panel->monitoring() && counts->samples>transient);
  auto *other=new QDockWidget("Other",&main);other->setWidget(new QWidget(other));main.addDockWidget(Qt::RightDockWidgetArea,other);
  main.tabifyDockWidget(dock,other);other->show();other->raise();run(100);
  const int tabbed=counts->samples;run(200);CHECK(!panel->monitoring() && counts->samples==tabbed);
  dock->raise();run(120);CHECK(panel->monitoring() && counts->samples>tabbed);
  fixture->rows[0].available=false;run(100);CHECK(model->data(model->index(0,2),Qt::DisplayRole).toString()=="Unavailable");
  for(int i=1;i<32;++i){auto extra=r;extra.id="fixture"+std::to_string(i);fixture->rows.push_back(extra);}
  run(100);CHECK(model->rowCount()==32);
  std::vector<double> paint;const auto before_paints=panel->test_paints();
  for(int i=0;i<100;++i) {
    for(auto &row:fixture->rows){row.available=true;row.value=.03+static_cast<double>(i%25)*.01;}
    QElapsedTimer t;t.start();panel->test_refresh();
    QCoreApplication::processEvents();QCoreApplication::sendPostedEvents(nullptr,QEvent::UpdateRequest);
    paint.push_back(t.nsecsElapsed()/1e6);
  }
  CHECK(panel->test_paints()>before_paints);
  std::sort(paint.begin(),paint.end());std::printf("32-row live snapshot/model/incremental-paint median/p95 %.3f/%.3f ms\n",paint[50],paint[95]);
  QImage image(panel->size(),QImage::Format_ARGB32_Premultiplied);panel->render(&image);
  if(argc>1)CHECK(image.save(QString::fromLocal8Bit(argv[1])));
  dock->hide();const int final=counts->samples;delete dock;run(160);CHECK(counts->samples==final);
  std::printf("%d dock lifecycle checks passed\n",checks);
}
